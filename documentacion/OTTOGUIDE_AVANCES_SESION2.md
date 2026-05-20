# OttoGuide IA — Avances Sesión 2
> Período: 18 al 19 de mayo de 2026  
> Continuación de `OTTOGUIDE_AVANCES_SESION1.md`

---

## RESUMEN EJECUTIVO

En esta sesión se completó el pipeline de voz de Otto de punta a punta:

```
Pregunta → Ollama (otto) → respuesta texto → Piper TTS → WAV → PlayStream → parlante del robot
```

Otto ahora piensa en español rioplatense con contexto de UADE y habla con voz masculina natural por su parlante.

---

## HITOS LOGRADOS

| Hito | Estado |
|---|---|
| Fix timing PlayStream (audio cortado) | ✓ Resuelto vía nano en Jetson |
| Volumen subido a 100 | ✓ |
| Amplificación WAV con ffmpeg (`volume=3.0`) | ✓ |
| espeak-ng instalado y probado | ✓ Voz robótica, solo para prueba |
| Piper TTS binario nativo aarch64 instalado | ✓ |
| Voz `es_MX-gevy-high` (masculina) descargada | ✓ Desde HirCoir/Piper-TTS-Spanish |
| Otto habla en español con voz natural | ✓ |
| Audio completo sin cortes | ✓ |
| Ollama instalado nativo con soporte JetPack GPU | ✓ |
| `gemma3:4b` descargado (~3.3GB) | ✓ |
| Modelfile corregido (`FROM gemma3:4b`) | ✓ |
| Modelo `otto` creado con personalidad UADE | ✓ |
| Loop completo: pregunta → Ollama → Piper → parlante | ✓ Validado |

---

## CAMBIOS TÉCNICOS IMPORTANTES

### 1. Fix de PlayStream — audio cortado
**Problema:** `PlayStop("otto")` se llamaba inmediatamente después del último chunk, cancelando el audio antes de terminar.  
**Fix:** Agregado `Sleep(duracion_audio + 2)` antes de `PlayStop` directamente en el Jetson con nano.

```cpp
double dur = (double)total / (16000.0 * 2.0);
unitree::common::Sleep((int)dur + 2);
ret = client.PlayStop("otto");
```

### 2. Volumen
`SAFE_VOLUME` cambiado de 50 → 100 en `otto_speak_file.cpp`.  
Amplificación adicional vía ffmpeg: `-af "volume=3.0"`.

### 3. Voz de Piper — ruta final
`es_MX-gevy-high` no estaba en `rhasspy/piper-voices` (HuggingFace devolvía 404).  
Ubicada en repo alternativo: `HirCoir/Piper-TTS-Spanish`.  
Nombre real del archivo: `es_MX-gevy-10196-epoch-high.onnx`.  
Guardado como: `~/piper/voices/es_MX-gevy-high.onnx`.

### 4. Modelfile — corrección de modelo base
El Modelfile original tenía `FROM gemma4:e4b` (no existe en Ollama).  
Corregido a `FROM gemma3:4b` con:
```bash
sed -i 's/FROM gemma4:e4b/FROM gemma3:4b/' ~/Desktop/Ollama-Ottoguide/Modelfile
```

### 5. Ollama — modelos en carpeta dedicada
Ollama configurado para guardar modelos en:
```
~/Desktop/Ollama-Ottoguide/models/
```
Variable de entorno agregada al servicio systemd:
```
Environment="OLLAMA_MODELS=/home/unitree/Desktop/Ollama-Ottoguide/models"
```

---

## ARQUITECTURA ACTUAL — LO QUE CORRE EN EL JETSON

```
~/piper/
  piper                          ← binario TTS nativo aarch64
  voices/
    es_MX-gevy-high.onnx        ← voz masculina español México
    es_MX-gevy-high.onnx.json

~/Desktop/Ollama-Ottoguide/
  Modelfile                      ← personalidad de Otto
  models/                        ← gemma3:4b (~3.3GB)

~/Desktop/teo_Ottoguide_IA/ottoguide-ia/
  src/otto_audio/cpp/
    otto_speak_file.cpp          ← binario de reproducción
    build/otto_speak_file        ← compilado aarch64

/usr/local/bin/ollama            ← binario Ollama (JetPack nativo)
/usr/local/bin/espeak-ng         ← TTS robótico (solo pruebas)
```

---

## LOOP COMPLETO VALIDADO

```bash
# Pregunta → Ollama → Piper → WAV → parlante
ollama run otto "¿Qué carreras tiene UADE?" --nowordwrap \
  | ~/piper/piper --model ~/piper/voices/es_MX-gevy-high.onnx --output_file /tmp/otto_resp.wav

ffmpeg -y -i /tmp/otto_resp.wav -ar 16000 -ac 1 -sample_fmt s16 -af "volume=3.0" /tmp/otto_resp_16k.wav

~/Desktop/teo_Ottoguide_IA/ottoguide-ia/src/otto_audio/cpp/build/otto_speak_file eth0 /tmp/otto_resp_16k.wav
```

---

## TABLA DE COMANDOS IMPORTANTES

| Comando | Dónde | Qué hace |
|---|---|---|
| `./otto_speak_file eth0 <archivo.wav>` | Jetson `/build/` | Reproduce WAV por el parlante del robot via SDK |
| `ollama run otto "pregunta"` | Jetson | Consulta al modelo otto con personalidad UADE |
| `ollama run otto "pregunta" --nowordwrap` | Jetson | Idem, sin saltos de línea (para pipe a Piper) |
| `ollama create otto -f Modelfile` | Jetson `Ollama-Ottoguide/` | Crea modelo otto desde el Modelfile |
| `ollama pull gemma3:4b` | Jetson | Descarga el modelo base (~3.3GB) |
| `ollama list` | Jetson | Lista modelos instalados |
| `echo "texto" \| ~/piper/piper --model <voz.onnx> --output_file out.wav` | Jetson | Genera WAV en español desde texto |
| `ffmpeg -y -i in.wav -ar 16000 -ac 1 -sample_fmt s16 -af "volume=3.0" out.wav` | Jetson | Convierte WAV a formato 16kHz mono 16-bit con amplificación |
| `espeak-ng -v es -s 130 "texto" -w out.wav` | Jetson | Genera WAV con voz robótica (solo pruebas) |
| `curl http://localhost:11434` | Jetson | Verifica que Ollama está corriendo |
| `sudo systemctl start ollama` | Jetson | Inicia el servicio Ollama |
| `sudo systemctl stop ollama` | Jetson | Detiene el servicio Ollama |
| `git pull` | Jetson repo | Trae cambios del notebook |
| `cmake .. && make -j4` | Jetson `build/` | Compila otto_speak_file para aarch64 |
| `ping 192.168.123.164` | Notebook | Verifica conectividad con el robot |
| `ssh unitree@192.168.123.164` | Notebook | Acceso SSH al Jetson del robot |
| `scp archivo unitree@192.168.123.164:~/destino/` | Notebook | Copia archivo al robot |

---

## INFRAESTRUCTURA — RESUMEN FINAL

| Componente | Ubicación | Estado |
|---|---|---|
| SDK2 C++ (aarch64) | `/opt/unitree_robotics/` | ✓ Instalado |
| SDK2 fuente | `~/unitree_sdk2/` | ✓ Compilado |
| Piper TTS | `~/piper/` | ✓ Funcionando |
| Voz gevy (es_MX) | `~/piper/voices/` | ✓ Activa |
| Ollama + JetPack GPU | `/usr/local/bin/ollama` | ✓ Corriendo |
| gemma3:4b | `~/Desktop/Ollama-Ottoguide/models/` | ✓ Descargado |
| Modelo otto | Ollama registry local | ✓ Creado |
| otto_speak_file | `~/Desktop/teo_Ottoguide_IA/.../build/` | ✓ Compilado |
| Repo GitHub (privado) | `ottoguide-ia` | ✓ Sincronizado |

---

## LO QUE FALTA

### Inmediato
- [ ] Sincronizar cambios del Jetson al repo (otto_speak_file.cpp con fix de timing y volumen)
- [ ] Crear script `otto_say.sh`: wrapper que une Piper + ffmpeg + otto_speak_file en un comando

### Core del pipeline
- [ ] Suscribirse a `rt/audio_msg` y leer transcripciones del ASR integrado
- [ ] Detectar wake word "Hola Otto" (comparación de texto con tolerancia Levenshtein)
- [ ] Conectar ASR → Ollama → Piper → parlante en loop continuo
- [ ] Manejo de estados: HIBERNACION / ESCUCHANDO / PROCESANDO

### Ejecutable final
- [ ] Programa C++ único que orqueste todo el pipeline
- [ ] Autostart al encender el Jetson (systemd service)
- [ ] Manejo de errores y reconexión automática

### Mejoras de calidad
- [ ] Ajustar velocidad y tono de voz Piper para sonar más natural
- [ ] Filtrar transcripciones con `is_final: true` y `confidence > 0.6`
- [ ] Limitar respuestas de Ollama a 2-3 oraciones máximo
- [ ] Detectar idioma `<|es|>` para ignorar ruido en otros idiomas

---

## PRÓXIMO PASO RECOMENDADO

Crear `otto_say.sh` — un wrapper que una todo el pipeline de TTS en un comando simple:

```bash
otto_say "Hola, bienvenidos a UADE."
# Internamente: Piper → ffmpeg → otto_speak_file
```

Eso simplifica el código C++ del pipeline principal y hace el sistema fácilmente testeable.

---

*Sesión del 19 de mayo de 2026. Otto habla, piensa y responde en español por primera vez.*
