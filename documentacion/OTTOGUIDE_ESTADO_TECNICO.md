# OttoGuide IA — Estado Técnico del Entorno
> Documento generado al cierre de la fase de setup y análisis.  
> Cubre: contexto del proyecto, entorno, arquitectura, decisiones, hallazgos del SDK y estado del setup.

---

## 1. CONTEXTO DEL PROYECTO

**Nombre:** OttoGuide IA  
**Origen:** Proyecto SIP — Grupo 6 — UADE (Universidad Argentina de la Empresa)  
**Campus:** Monserrat, Buenos Aires  
**Objetivo final:** Robot humanoide Unitree G1-EDU como guía interactivo del campus.  
**Pipeline de conversación objetivo:**

```
"Hola Otto" (wake word)
  → STT (transcripción)
  → LLM local (razonamiento en español rioplatense)
  → TTS (síntesis de voz)
  → Parlante del robot
```

**Personalidad del LLM (modelo "otto"):**
- Responde en español rioplatense
- Respuestas cortas (máx. 3 oraciones)
- Sin emojis
- Siempre menciona "UADE" como sigla
- Modelo base: `gemma4:e4b` + Modelfile de personalidad

---

## 2. HARDWARE

### Robot — Unitree G1-EDU
| Atributo | Valor |
|---|---|
| Modelo | Unitree G1-EDU (humanoide) |
| Computadora onboard | NVIDIA Jetson Orin NX 16GB |
| OS | Ubuntu 20.04.6 LTS |
| JetPack | R35.3.1 |
| Arquitectura | aarch64 |
| ROS | ROS2 Foxy |
| IP | 192.168.123.164 |
| Usuario | `unitree` / `123` |
| Interfaz de red SDK | `eth0` |

### Notebook de desarrollo — Desktop-H3xt
| Atributo | Valor |
|---|---|
| Host | ThinkPad L14 Gen 1 (20U5004RCA) |
| OS | Linux Mint 22.2 x86_64 |
| Kernel | 6.17.0-23-generic |
| CPU | AMD Ryzen 5 PRO 4650U (12 cores) |
| RAM | 31 GB |
| Shell | zsh 5.9 |
| Terminal | kitty |
| Usuario | `h3xt` |

---

## 3. ENTORNO DE DESARROLLO — DECISIONES

### 3.1 Incompatibilidad crítica ROS2

| Sistema | Base Ubuntu | ROS2 compatible |
|---|---|---|
| Notebook (Mint 22.2) | Ubuntu 24.04 (Noble) | **Foxy NO disponible** |
| Robot (Jetson) | Ubuntu 20.04 | Foxy nativo ✓ |

**Decisión:** No instalar ROS2 nativo en el notebook.  
**Alternativas evaluadas:**

| Opción | Estado |
|---|---|
| Docker `osrf/ros:foxy-desktop` | Disponible para validación de nodos |
| Desarrollar sin ROS2 localmente | **Estrategia principal** |
| ROS2 Jazzy/Humble en notebook | Descartado (incompatible con Foxy del robot) |

### 3.2 SDK Python descartado en notebook

`unitree_sdk2_python` requiere `cyclonedds==0.10.2`. No existe wheel pre-compilada para Python 3.12. Compilar desde fuente falla porque el subproceso de `pip` no hereda `CYCLONEDDS_HOME`.

```
ERROR: Could not find a version that satisfies the requirement cyclonedds==0.10.2
       (from versions: 11.0.1)
```

**Decisión:** SDK Python solo se instala en el **Jetson** (Ubuntu 20.04, Python 3.8/3.10).  
**Impacto:** Nulo. Los nodos Python (`otto_brain`, `otto_stt`) usan `rclpy` + `requests` — no importan `unitree_sdk2_python` directamente.

### 3.3 SDK C++ instalado en notebook (x86_64)

El SDK2 incluye binarios pre-compilados para **ambas arquitecturas**:
```
lib/x86_64/libunitree_sdk2.a   ← notebook
lib/aarch64/libunitree_sdk2.a  ← Jetson (robot)
```

CMake detecta la arquitectura automáticamente. Build exitoso en el notebook.

### 3.4 Workflow de desarrollo

```
Notebook (editar + build x86_64)
  │
  ├── rsync src/ → /home/unitree/ottoguide/
  │
  └── SSH → Robot Jetson
               ├── cmake + make (aarch64)
               └── ejecutar binarios con hardware real
```

---

## 4. ESTADO DEL SETUP (al cierre de esta fase)

| Paso | Descripción | Estado |
|---|---|---|
| 0.1 | Deps C++ (`cmake`, `boost`, `yaml-cpp`, `eigen3`, etc.) | ✓ OK |
| 0.2 | CycloneDDS compilado | ✓ OK |
| 0.3 | SDK2 C++ compilado e instalado en `/opt/unitree_robotics/` | ✓ OK |
| 0.4 | SDK2 Python en notebook | ✗ Descartado (ver §3.2) |
| 0.5 | Docker ROS2 Foxy | Pendiente (no requerido aún) |
| 0.6 | Script `deploy_to_robot.sh` | Pendiente |
| — | Conectividad al robot | ✓ ping 0% loss ~0.2ms RTT |

### Rutas de instalación relevantes

```
/opt/unitree_robotics/                       ← SDK2 C++ instalado
  include/unitree/robot/g1/audio/
    g1_audio_client.hpp                      ← API AudioClient
    g1_audio_api.hpp                         ← IDs de API y parámetros
  lib/
    libunitree_sdk2.a
    libddsc.so / libddscxx.so
  lib/cmake/unitree_sdk2/                    ← find_package() path

~/Escritorio/Ottoguide_IA/
  cyclonedds/install/                        ← CycloneDDS compilado
  sdk/
    unitree_sdk2/                            ← SDK fuente + ejemplos
      example/g1/audio/
        g1_audio_client_example.cpp          ← ejemplo oficial de audio
        wav.hpp                              ← lector/escritor WAV
      bin/
        g1_audio_client_example             ← binario ya compilado
    unitree_sdk2_python/                     ← SDK Python (no instalado)
  src/
    otto_audio/
      cpp/                                   ← código C++ del proyecto
      python/                                ← nodos Python
      launch/                                ← launch files ROS2
```

---

## 5. ECOSISTEMA UNITREE — REPOS RELEVANTES

Organización GitHub: `unitreerobotics` (43 repos públicos)

### Repos utilizados en este proyecto

| Repo | Rol |
|---|---|
| `unitree_sdk2` | SDK C++ principal. Comunicación DDS. AudioClient. **Core de todo.** |
| `unitree_sdk2_python` | Wrapper Python. Solo en Jetson. |
| `unitree_ros2` | Integración ROS2. DDS compartido con SDK2. |
| `unitree_mujoco` | Simulador para sim2real. Opcional en etapas futuras. |

### Protocolo de comunicación

Todo el ecosistema usa **CycloneDDS `0.10.2`** sobre UDP. ROS2 y SDK2 son directamente compatibles porque comparten el mismo DDS.

```
SDK2 ←→ CycloneDDS 0.10.2 ←→ ROS2 Foxy
```

---

## 6. SISTEMA DE AUDIO DEL G1 — HALLAZGOS COMPLETOS

### 6.1 Micrófono — captura de audio

**El audio del micrófono NO llega por DDS.** Llega por **UDP multicast**:

```
Grupo multicast : 239.168.123.161
Puerto          : 5555
Formato         : PCM int16, 16kHz, mono, raw (sin header)
Recepción       : recvfrom() en socket UDP
Chunk size      : WAV_LEN_ONCE = 16000 * 2 * 160 / 1000 = 5120 bytes
```

Esto resuelve el "bloqueante crítico" del roadmap original que asumía PCM en `rt/audio_msg`.

### 6.2 ASR integrado — `rt/audio_msg`

El tópico DDS `rt/audio_msg` publica **texto transcripto** (resultado ASR), **no audio crudo**.

```cpp
// Tipo: std_msgs::msg::dds_::String_
// Tópico: "rt/audio_msg"
// Contenido: string con el texto reconocido por el ASR del robot
ChannelSubscriber<std_msgs::msg::dds_::String_> subscriber("rt/audio_msg");
subscriber.InitChannel(asr_handler);
// callback recibe: resMsg->data() → texto en string
```

**Implicación:** El robot tiene ASR integrado. Whisper puede eliminarse del MVP.

### 6.3 API AudioClient — firmas exactas

```cpp
// Inicialización
unitree::robot::ChannelFactory::Instance()->Init(0, "eth0");
unitree::robot::g1::AudioClient client;
client.SetTimeout(10.0f);
client.Init();

// Volumen
int32_t GetVolume(uint8_t &volume);          // ret=0 si OK
int32_t SetVolume(uint8_t volume);           // 0–100

// TTS nativo (chino/inglés ÚNICAMENTE — NO español)
int32_t TtsMaker(const std::string &text, int32_t speaker_id);
//   speaker_id=0 → chino
//   speaker_id=1 → inglés
//   Para español: usar Piper + PlayStream

// Reproducción de PCM
int32_t PlayStream(std::string app_name,
                   std::string stream_id,
                   std::vector<uint8_t> pcm_data);
// - PCM: 16kHz, mono, 16-bit signed, little-endian (uint8_t pairs)
// - Chunks recomendados: 96000 bytes (~3 segundos)
// - Sleep(1) entre chunks
// - stream_id = timestamp en ms como string

int32_t PlayStop(std::string app_name);
// OJO: toma app_name, NO stream_id
// Bug en ejemplo oficial: pasa stream_id pero el header espera app_name

// LEDs
int32_t LedControl(uint8_t R, uint8_t G, uint8_t B);
```

### 6.4 IDs de API

```cpp
const int32_t ROBOT_API_ID_AUDIO_TTS        = 1001;
const int32_t ROBOT_API_ID_AUDIO_ASR        = 1002;
const int32_t ROBOT_API_ID_AUDIO_START_PLAY = 1003;
const int32_t ROBOT_API_ID_AUDIO_STOP_PLAY  = 1004;
const int32_t ROBOT_API_ID_AUDIO_GET_VOLUME = 1005;
const int32_t ROBOT_API_ID_AUDIO_SET_VOLUME = 1006;
const int32_t ROBOT_API_ID_AUDIO_SET_RGB_LED= 1010;
const std::string AUDIO_SERVICE_NAME        = "voice";
const std::string AUDIO_API_VERSION         = "1.0.0.0";
```

### 6.5 wav.hpp — lector WAV oficial

Ubicación: `example/g1/audio/wav.hpp`  
Copiado a: `src/otto_audio/cpp/wav.hpp`

```cpp
// Leer WAV → vector<uint8_t> PCM
std::vector<uint8_t> ReadWave(const std::string &filename,
                               int32_t *sampling_rate,
                               int8_t  *channelCount,
                               bool    *is_ok);

// Validaciones internas del lector:
// - formato RIFF WAVE PCM (audio_format=1)
// - 16-bit por sample
// - byte_rate y block_align consistentes
// - maneja chunks JUNK y subchunk1_size=18

// Escribir WAV desde PCM int16
bool WriteWave(const std::string &filename,
               int32_t sampling_rate,
               const int16_t *samples,
               int32_t n,
               uint8_t num_channels);
```

---

## 7. ARQUITECTURA DE NODOS — VERSIÓN REVISADA

### Pipeline real (post-análisis del SDK)

```
┌────────────────────────────────────────────────────────────────────┐
│                        ROBOT G1-EDU (Jetson)                        │
│                                                                      │
│  [Micrófonos]                                                        │
│      │ UDP multicast 239.168.123.161:5555 PCM int16 16kHz          │
│      ↓                                                               │
│  [otto_audio_capture] (C++)                                          │
│      ├── Opción A: recvfrom() → /otto/audio_raw → Whisper (opt.)   │
│      └── Opción B (MVP): ASR integrado via rt/audio_msg → texto    │
│                                          ↓                           │
│  [/otto/stt_text] std_msgs/String                                   │
│      ↓                                                               │
│  [otto_brain] (Python)                                               │
│      ├── detecta wake word "Hola Otto" (Levenshtein dist ≤2)       │
│      ├── estado: HIBERNACION / ESCUCHANDO                           │
│      └── POST http://localhost:11434/api/generate model="otto"      │
│                                          ↓                           │
│  [/otto/llm_response] std_msgs/String                               │
│      ↓                                                               │
│  [otto_tts_bridge] (C++)                                             │
│      ├── Piper Docker → WAV 16kHz mono 16-bit                       │
│      ├── ReadWave() → vector<uint8_t>                               │
│      ├── PlayStream("otto", stream_id, chunk) × N                  │
│      └── PlayStop("otto")                                            │
│                                          ↓                           │
│  [Parlante del robot]                                                │
└────────────────────────────────────────────────────────────────────┘
```

### Tópicos ROS2

| Tópico | Tipo | Productor | Consumidor |
|---|---|---|---|
| `/otto/audio_raw` | `std_msgs/UInt8MultiArray` | `otto_audio_capture` | `otto_stt` (opcional) |
| `/otto/stt_text` | `std_msgs/String` | `otto_audio_capture` o `otto_stt` | `otto_brain` |
| `/otto/llm_response` | `std_msgs/String` | `otto_brain` | `otto_tts_bridge` |
| `/otto/state` | `std_msgs/String` | `otto_brain` | monitoreo |

---

## 8. SERVICIOS DOCKER (en el Jetson)

| Servicio | Imagen | Puerto | Config |
|---|---|---|---|
| Whisper STT | `onerahmet/openai-whisper-asr-webservice` | 9000 | `ASR_MODEL=medium`, `ASR_ENGINE=faster_whisper` |
| Ollama LLM | `ollama/ollama` | 11434 | modelo `otto` = `gemma4:e4b` + Modelfile |
| Piper TTS | build local desde `services/tts/Dockerfile` | — | voz `es_MX-gevy-high.onnx` |

**Piper:** no usar wyoming service. Ejecutar como `tail -f /dev/null` y llamar vía `docker exec`.

```bash
# Crear modelo otto en Ollama
docker exec -it ottoguide-llm ollama pull gemma4:e4b
docker cp services/llm/Modelfile ottoguide-llm:/tmp/Modelfile
docker exec -it ottoguide-llm ollama create otto -f /tmp/Modelfile
```

---

## 9. ROADMAP DE IMPLEMENTACIÓN

| Fase | Dónde | Objetivo | Criterio de éxito | Riesgo |
|---|---|---|---|---|
| 0 | Notebook | Setup SDK C++ + CycloneDDS | `libunitree_sdk2.a` presente | Bajo |
| 1 | Robot SSH | Auditoría read-only | Estado conocido | Bajo |
| **2** | **Robot** | **`GetVolume` + `SetVolume`** | **ret=0, robot responde** | **Medio** |
| **3** | **Robot** | **`otto_speak_file` → parlante** | **Robot reproduce WAV** | **Alto** |
| 4 | Robot | `otto_say` CLI | texto → voz estable | Medio |
| 5 | Robot | Nodo ROS2 `otto_tts_bridge` | `ros2 topic pub` → robot habla | Medio |
| 6 | Robot | Conectar Ollama | Respuestas orales del LLM | Medio |
| 7 | Robot | Micrófono UDP + wake word | Conversación completa | Alto |

### Criterio de MVP completado

```
1. Robot reproduce frase española por parlante via AudioClient.PlayStream()
2. Frase generada desde texto con Piper (es_MX-gevy-high.onnx)
3. Audio normalizado a 16kHz, mono, 16-bit PCM
4. Proceso ejecutable desde CLI o nodo ROS2
5. Volumen controlable (SetVolume)
6. Cero comandos de movimiento ejecutados
7. Cambio documentado y reproducible
```

---

## 10. RESTRICCIONES OPERATIVAS PERMANENTES

```
PROHIBIDO sin autorización explícita:
  - Comandos de locomoción (SportClient, LowCmd, LowState, articulaciones)
  - git push / git commit
  - rm -rf
  - sudo en el robot salvo instalación explícitamente solicitada
  - Modificar /libs salvo pedido explícito
  - Reiniciar servicios del sistema del robot
  - Instalar paquetes globales sin proponer primero

VOLUMEN DE AUDIO:
  - Máximo 50 durante pruebas
  - Nunca 100 sin estar preparado

DDS DOMAIN ID:
  - DomainID 0 → reservado para servicios del robot
  - DomainID 1 → usar para pruebas desde notebook
  - En el Jetson para producción: DomainID 0, interfaz eth0
```

---

## 11. CÓDIGO LISTO PARA COMPILAR

### `src/otto_audio/cpp/otto_speak_file.cpp`
Binario C++ mínimo: WAV → `AudioClient.PlayStream()` → parlante del G1.  
- Valida conectividad con `GetVolume()` antes de reproducir  
- Fija volumen a 50 de forma segura  
- Streams en chunks de 96000 bytes con Sleep(1) entre cada uno  
- `PlayStop("otto")` al finalizar  
- Errores descriptivos con instrucción de conversión `ffmpeg`

### `src/otto_audio/cpp/CMakeLists.txt`
- `find_package(unitree_sdk2)` desde `/opt/unitree_robotics/lib/cmake/unitree_sdk2`
- Target: `otto_speak_file`
- C++17, Release

### `src/otto_audio/cpp/wav.hpp`
Copiado de `sdk/unitree_sdk2/example/g1/audio/wav.hpp`.

---

## 12. PRÓXIMAS PRUEBAS EN EL ROBOT REAL

Secuencia segura recomendada antes de cualquier código nuevo:

```bash
# 1. Verificar SSH
ssh unitree@192.168.123.164

# 2. Auditoría read-only del estado del robot
hostname && whoami && uname -a
docker ps
source /opt/ros/foxy/setup.bash && ros2 --version
find ~/Desktop -maxdepth 4 -type d -iname "*unitree*" 2>/dev/null
find ~/Desktop -maxdepth 4 -name "docker-compose.yml" 2>/dev/null
find ~/Desktop -maxdepth 5 -name "*.onnx" 2>/dev/null

# 3. Compilar otto_speak_file en el Jetson (aarch64)
# (después de rsync del src/)

# 4. Primera prueba de audio
./otto_speak_file eth0 /tmp/otto_test.wav
# Salida esperada: [OK] Conectado. Volumen actual: XX
```

---

*Documento generado al cierre del setup inicial. Última acción cubierta: análisis completo del código fuente de AudioClient y wav.hpp del SDK2 oficial.*
