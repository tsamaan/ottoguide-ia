# OttoGuide IA

Robot guía conversacional para el campus Monserrat de UADE.  
Plataforma: Unitree G1-EDU · Jetson Orin NX 16GB · Ubuntu 20.04

---

## Stack

| Componente | Tecnología | Ubicación en el Jetson |
|---|---|---|
| Captura de audio | UDP Multicast 239.168.123.161:5555 | SDK nativo |
| STT | whisper.cpp large-v3-turbo + CUDA | `~/Desktop/whisper.cpp/` |
| LLM | Ollama gemma3:4b + modelo otto | `~/Desktop/Ollama-Ottoguide/` |
| TTS | Piper es_MX-gevy-high (voz masculina) | `~/piper/` |
| Reproducción | AudioClient.PlayStream() via SDK2 | `/opt/unitree_robotics/` |

---

## Red

```
192.168.123.100  Notebook desarrollo   (ThinkPad, Linux Mint)
192.168.123.161  Main Controller G1    (servicios: voice, DDS)
192.168.123.164  Jetson Orin NX        (deploy y ejecución)
```

Conexión: `ssh unitree@192.168.123.164` · contraseña: `123`

---

## Flujo de desarrollo

```
1. Editar código en VS Code (notebook)
2. git commit + git push (notebook)
3. git pull en el Jetson (SSH)
4. Compilar en el Jetson
5. Probar en el robot
```

---

## Comandos — actualización completa

### 1. Desde el notebook — subir cambios

```bash
cd ~/Escritorio/Ottoguide_IA
git add src/
git commit -m "descripcion del cambio"
git push
```

### 2. Desde el Jetson — bajar y compilar

```bash
# Conectarse
ssh unitree@192.168.123.164

# Bajar cambios
cd ~/Desktop/teo_Ottoguide_IA/ottoguide-ia
git pull

# Compilar
cd src/otto_audio/cpp/build
/home/unitree/.local/lib/python3.8/site-packages/cmake/data/bin/cmake ..
make -j4 2>&1 | grep -E "error:|otto_pipeline|%"
```

### 3. Ejecutar el pipeline

```bash
~/Desktop/teo_Ottoguide_IA/ottoguide-ia/src/otto_audio/cpp/build/otto_pipeline
```

Esperá el banner verde y decí **"Hola Otto"** para activar.

---

## Comandos — pruebas individuales

### Probar solo TTS (sin pipeline)

```bash
# Volúmenes disponibles: bajo | medio | alto | max
~/Desktop/teo_Ottoguide_IA/ottoguide-ia/src/otto_audio/scripts/otto_say.sh "texto a reproducir" alto
```

### Probar solo Whisper (transcribir un WAV)

```bash
~/Desktop/whisper.cpp/build/bin/whisper-cli \
  -m ~/Desktop/whisper.cpp/models/ggml-large-v3-turbo.bin \
  -l es -nt \
  --prompt "UADE Otto OttoGuide" \
  -f /ruta/al/audio.wav
```

### Probar solo Ollama

```bash
ollama run otto "tu pregunta aqui"
```

### Probar solo AudioClient (reproducir WAV por el parlante)

```bash
~/Desktop/teo_Ottoguide_IA/ottoguide-ia/src/otto_audio/cpp/build/otto_speak_file eth0 /ruta/al/audio.wav
```

### Ver transcripción del ASR del robot en tiempo real

```bash
~/Desktop/teo_Ottoguide_IA/ottoguide-ia/src/otto_audio/cpp/build/asr_test eth0
```

### Generar WAV de prueba (tono 440Hz)

```bash
python3 -c "
import wave, struct, math, os
path='/tmp/test_tono.wav'; rate=16000; dur=2
s=[int(32767*math.sin(2*math.pi*440*i/rate)) for i in range(rate*dur)]
with wave.open(path,'w') as f:
    f.setnchannels(1); f.setsampwidth(2); f.setframerate(rate)
    f.writeframes(b''.join(struct.pack('<h',x) for x in s))
print('OK:', path)
"
```

---

## Comandos — servicios

### Verificar que Ollama está corriendo

```bash
curl -s http://localhost:11434
# Respuesta esperada: Ollama is running
```

### Iniciar/reiniciar Ollama

```bash
sudo systemctl start ollama
sudo systemctl status ollama
```

### Ver modelos disponibles

```bash
ollama list
```

### Recrear modelo otto desde Modelfile

```bash
ollama create otto -f ~/Desktop/Ollama-Ottoguide/Modelfile
```

---

## Archivos del proyecto

```
src/
  otto_audio/
    cpp/
      otto_pipeline.cpp    ← programa principal (pipeline completo)
      otto_speak_file.cpp  ← reproducir WAV por parlante
      asr_test.cpp         ← escuchar ASR del robot en tiempo real
      CMakeLists.txt       ← configuración de build
      wav.hpp              ← lector WAV (del SDK de Unitree)
      build/               ← binarios compilados (no versionar)
    python/
      mic_capture.py       ← capturar audio del mic a WAV
    scripts/
      otto_say.sh          ← wrapper TTS: texto → voz → parlante
    launch/
      (vacío — launch files ROS2 futuros)

documentacion/
  OTTOGUIDE_ESTADO_TECNICO.md     ← entorno, arquitectura, decisiones
  OTTOGUIDE_AVANCES_SESION1.md    ← sesión 1: primer audio, SDK
  OTTOGUIDE_AVANCES_SESION2.md    ← sesión 2: TTS español, Ollama
  OTTOGUIDE_AVANCES_SESION3.html  ← sesión 3: pipeline completo
```

---

## cmake — nota importante

El sistema cmake del Jetson es 3.16 (incompatible con whisper.cpp CUDA).  
**Siempre usar el cmake instalado via pip:**

```bash
/home/unitree/.local/lib/python3.8/site-packages/cmake/data/bin/cmake ..
```

Nunca usar solo `cmake ..` en el Jetson para este proyecto.

---

## Constantes configurables en otto_pipeline.cpp

| Constante | Valor actual | Descripción |
|---|---|---|
| `RMS_THRESHOLD` | 1300 | Nivel mínimo de amplitud para procesar audio |
| `CAPTURE_SECS` | 3 | Segundos de captura en HIBERNACION |
| `TIMEOUT_SECS` | 30 | Segundos sin voz para volver a HIBERNACION |
| `SDK_VOLUME` | 70 | Volumen del parlante (0-100) |
| `WHISPER_PROMPT` | "UADE Otto OttoGuide" | Contexto para el modelo Whisper |

Para cambiar una constante: editar en notebook → push → pull en Jetson → compilar → probar.

---

## Wake words reconocidas

```
"Hola Otto"  /  "Ola Otto"  /  "Hola Oto"  /  "Hola Auto"
```

## Palabras de despedida reconocidas

```
"Chau"  /  "Adiós"  /  "Hasta luego"  /  "Hasta pronto"  /  "Listo"
```

---

## Estados del pipeline

```
HIBERNACION  →  escucha wake word, ignora el resto
ESCUCHANDO   →  captura preguntas, consulta Ollama, responde
PROCESANDO   →  esperando respuesta de Ollama (transitorio)
```

---

## Troubleshooting

**El robot no responde al AudioClient**
```bash
# Verificar conectividad
ping 192.168.123.164
# El SDK solo funciona ejecutado desde el Jetson con eth0
# Nunca desde el notebook
```

**Whisper no detecta GPU**
```bash
# Verificar que se compiló con CUDA
~/Desktop/whisper.cpp/build/bin/whisper-cli --help | grep -i cuda
# Si no hay GPU: recompilar con -DGGML_CUDA=1 -DCMAKE_CUDA_ARCHITECTURES=87
```

**Ollama no responde**
```bash
sudo systemctl restart ollama
curl -s http://localhost:11434
```

**Audio cortado al final**
- Verificar que `otto_pipeline` está usando la versión compilada más reciente.
- El Sleep de duración + 2 segundos antes de PlayStop es intencional.

**Alucinaciones de Whisper**
- Agregar el patrón a `PATRONES[]` en `es_alucinacion()`.
- Si RMS es bajo y aparecen alucinaciones: subir `RMS_THRESHOLD`.