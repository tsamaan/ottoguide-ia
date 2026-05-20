# OttoGuide IA — Avances Sesión 1
> Continuación de `OTTOGUIDE_ESTADO_TECNICO.md`  
> Cubre: compilación en Jetson, primer contacto con hardware de audio, hallazgos del SDK, bugs encontrados y estado actual.

---

## 1. RESUMEN DE LO LOGRADO

| Hito | Estado |
|---|---|
| SDK2 C++ compilado en notebook (x86_64) | ✓ |
| SDK2 C++ compilado en Jetson (aarch64) | ✓ |
| `otto_speak_file` compilado en Jetson | ✓ |
| `GetVolume` responde desde Jetson | ✓ ret=0 |
| `SetVolume` funciona | ✓ ret=0 |
| `PlayStream` envía datos sin error | ✓ ret=0 |
| Robot habla por el parlante | ✓ via TtsMaker |
| Micrófono captura audio | ✓ ASR activo |
| `rt/audio_msg` formato completo conocido | ✓ JSON rico |
| Repo GitHub privado creado y sincronizado | ✓ |

---

## 2. SETUP EN EL JETSON — LO QUE SE INSTALÓ

### Rutas relevantes en el Jetson

```
/opt/unitree_robotics/              ← SDK2 instalado (aarch64)
  include/unitree/robot/g1/audio/
    g1_audio_client.hpp
    g1_audio_api.hpp
  lib/
    libunitree_sdk2.a               ← aarch64
    libddsc.so / libddscxx.so

~/unitree_sdk2/                     ← fuente del SDK (NO borrar)
  build/bin/
    g1_audio_client_example         ← binario oficial de audio compilado

~/Desktop/teo_Ottoguide_IA/
  ottoguide-ia/                     ← repo del proyecto (git clone)
    src/otto_audio/cpp/
      build/
        otto_speak_file             ← binario del proyecto compilado
```

### Dependencias instaladas en el Jetson

Todo ya estaba instalado salvo `libfmt-dev`. Paquetes confirmados:
`cmake`, `g++`, `build-essential`, `libyaml-cpp-dev`, `libboost-all-dev`, `libeigen3-dev`, `libspdlog-dev`, `libfmt-dev`

---

## 3. RESULTADO DEL PRIMER TEST — otto_speak_file

### Desde el notebook (FALLÓ)

```
[ERROR] GetVolume ret=3104
```

**Causa:** `ret=3104` = timeout DDS. El servicio `voice` corre en el **main controller** (`192.168.123.161`), no en el Jetson. Desde el notebook (`192.168.123.100`), el DDS no alcanza el servicio correctamente. El SDK está diseñado para correr **desde el Jetson**, no desde máquinas externas.

### Desde el Jetson (OK)

```
[OK] Conectado. Volumen actual: 100
[INFO] Volumen fijado a 50
[OK] WAV: 64000 bytes | 16000Hz | mono
[INFO] Reproduciendo stream=1778803883837
[INFO] chunk=64000 offset=0 ret=0
[OK] PlayStop ret=0
```

Todos los `ret=0`. El SDK funciona correctamente desde el Jetson con `eth0`.

**Sin audio escuchado** — ver §5 (bug identificado).

---

## 4. ARQUITECTURA DE RED CONFIRMADA

```
192.168.123.100  ← Notebook (desarrollo) — NO puede llamar AudioClient
192.168.123.161  ← Main Controller del G1 (corre servicios: voice, motion, etc.)
192.168.123.164  ← Jetson Orin NX / PC4 (desde aquí se llama al AudioClient)
```

**Regla fija:** Todo código que use `AudioClient` (o cualquier cliente del SDK2) debe ejecutarse **desde el Jetson** con interfaz `eth0`.

El notebook se usa para:
- Editar código
- Push al repo
- Pull en el Jetson vía SSH
- Compilar en el Jetson vía SSH

---

## 5. BUG IDENTIFICADO — PlayStream sin audio

### Síntoma
`PlayStream ret=0` y `PlayStop ret=0` pero el robot no emite sonido.

### Causa
`PlayStop("otto")` se llama **inmediatamente** después de enviar el último chunk de PCM, antes de que el robot termine de reproducirlo. El audio se cancela antes de escucharse.

### Fix implementado
Calcular la duración del audio y hacer `Sleep(duration + 1)` antes de `PlayStop`:

```cpp
// duracion = bytes / (16000 Hz * 2 bytes/sample) + 1s buffer
double duration_s = (double)total / (16000.0 * 2.0);
unitree::common::Sleep((int)duration_s + 1);
ret = client.PlayStop("otto");
```

**Estado del fix:** Escrito en notebook, pusheado al repo. Pendiente de compilar y probar en el Jetson en próxima sesión.

---

## 6. ROBOT HABLÓ — via g1_audio_client_example

Corriendo el ejemplo oficial desde el Jetson:

```bash
~/unitree_sdk2/build/bin/g1_audio_client_example eth0
```

Salida:
```
GetVolume API ret:0  volume = 50
SetVolume to 100% , API ret:0
TtsMaker API ret:0
Topic:"rt/audio_msg" recv: {"play_state":1}
Topic:"rt/audio_msg" recv: {"play_state":0}
TtsMaker API ret:0
...
```

**El robot habló por su parlante.** El idioma fue chino (speaker_id=0) e inglés (speaker_id=1), que son los únicos soportados por el TtsMaker nativo. Para español se necesita Piper + PlayStream (camino confirmado del roadmap).

---

## 7. HALLAZGOS CRÍTICOS — rt/audio_msg

### El tópico es JSON rico, no texto plano

Durante la ejecución del ejemplo, los micrófonos captaron ruido ambiental y el ASR lo transcribió en tiempo real:

```json
{
  "index": 13,
  "timestamp": 1778803269435,
  "type": 0,
  "text": "ですよ。",
  "angle": 0,
  "speaker_id": 0,
  "emotion": "<|EMO_UNKNOWN|>",
  "confidence": 0.500000,
  "language": "<|ja|>",
  "is_final": false
}
```

### Campos disponibles

| Campo | Tipo | Descripción |
|---|---|---|
| `index` | int | Número de secuencia de la transcripción |
| `timestamp` | long | Timestamp en milisegundos |
| `text` | string | Texto transcripto |
| `angle` | float | Ángulo de dirección del hablante |
| `speaker_id` | int | ID del hablante detectado |
| `emotion` | string | Emoción detectada (Whisper tag) |
| `confidence` | float | Confianza de la transcripción (0–1) |
| `language` | string | Idioma detectado en formato Whisper |
| `is_final` | bool | Si es transcripción final o parcial |

### Códigos de idioma Whisper detectados

| Código | Idioma |
|---|---|
| `<\|yue\|>` | Cantonés |
| `<\|ja\|>` | Japonés |
| `<\|en\|>` | Inglés |
| `<\|es\|>` | Español ← **esperado para Otto** |
| `<\|zh\|>` | Chino mandarín |

### play_state — saber cuándo termina el audio

```json
{"play_state": 1}   ← audio comenzó a reproducirse
{"play_state": 0}   ← audio terminó de reproducirse
```

Este mecanismo permite sincronizar reproducción en lugar de usar `Sleep` fijo.

### Implicación para el pipeline

El ASR integrado del robot **ya funciona y está activo**. Es Whisper-based y soporta múltiples idiomas incluyendo español. Esto confirma que el nodo STT puede suscribirse directamente a `rt/audio_msg` usando:

```cpp
// @CONTEXT: tipo confirmado del mensaje rt/audio_msg
ChannelSubscriber<std_msgs::msg::dds_::String_> subscriber("rt/audio_msg");
subscriber.InitChannel([](const void* msg) {
    auto* m = (std_msgs::msg::dds_::String_*)msg;
    // m->data() contiene JSON string completo
    // parsear con nlohmann/json o similar
});
```

---

## 8. ESTADO DEL REPO

```
repo: github.com/TU_USUARIO/ottoguide-ia (privado)
rama: main
```

Archivos en el repo:

```
.gitignore
documentacion/
  OTTOGUIDE_ESTADO_TECNICO.md       ← documentación anterior
src/
  otto_audio/
    cpp/
      CMakeLists.txt
      otto_speak_file.cpp            ← incluye fix de timing (no probado aún)
      wav.hpp
    python/
      mic_capture.py
    launch/                          ← vacío por ahora
```

---

## 9. PENDIENTE PARA PRÓXIMA SESIÓN

| Tarea | Prioridad | Descripción |
|---|---|---|
| Probar fix PlayStream | Alta | `git pull` en Jetson + recompilar + correr con tono 440Hz |
| Confirmar audio PlayStream | Alta | El robot debe emitir el tono por el parlante |
| Probar mic_capture.py | Alta | Capturar audio desde UDP multicast y guardar WAV |
| Instalar Docker en Jetson | Media | Para levantar Piper TTS y Ollama |
| Probar Piper → PlayStream | Alta | Generar WAV español y reproducirlo |
| Configurar ASR para español | Media | Verificar que `rt/audio_msg` detecta `<\|es\|>` correctamente |

---

## 10. WORKFLOW ESTABLECIDO

```
1. Editar código en el notebook
2. git commit + git push desde el notebook
3. ssh unitree@192.168.123.164
4. cd ~/Desktop/teo_Ottoguide_IA/ottoguide-ia
5. git pull
6. cd src/otto_audio/cpp/build
7. cmake .. && make -j4
8. ./otto_speak_file eth0 <archivo.wav>
```

---

## 11. RESTRICCIONES OPERATIVAS (vigentes)

```
- Nunca correr AudioClient desde el notebook (ret=3104, no funciona)
- Siempre compilar en el Jetson para producción (aarch64)
- Volumen máximo durante pruebas: 50
- No ejecutar comandos de movimiento (SportClient, LowCmd)
- No hacer git push desde el Jetson (solo git pull)
- El Jetson es destino de deploy, no de desarrollo
```

---

*Documento generado al cierre de la primera sesión de pruebas con hardware real.*  
*El robot habló por primera vez usando el SDK oficial.*
