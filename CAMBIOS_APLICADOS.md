# OttoGuide: Cambios Aplicados a otto_pipeline.cpp y otto_say.sh

Fecha: 20 de mayo de 2026

## CAMBIO 1: Limpiar buffer después de otto_say()

**ARCHIVO:** src/otto_audio/cpp/otto_pipeline.cpp  
**FUNCIÓN:** `otto_say()`  
**LÍNEA:** ~153 (final de la función)

**ANTES:**
```cpp
    double dur = (double)total / (16000.0 * 2.0);
    unitree::common::Sleep((int)dur + 2);
    g_audio->PlayStop("otto");
}
```

**DESPUÉS:**
```cpp
    double dur = (double)total / (16000.0 * 2.0);
    unitree::common::Sleep((int)dur + 2);
    g_audio->PlayStop("otto");
    // Limpiar buffer para evitar que la voz de Otto se transcriba como pregunta
    {
        std::lock_guard<std::mutex> lock(buf_mutex);
        audio_buffer.clear();
    }
}
```

---

## CAMBIO 2: Mejorar filtro anti-alucinaciones

**ARCHIVO:** src/otto_audio/cpp/otto_pipeline.cpp  
**FUNCIÓN:** `es_alucinacion()`  
**LÍNEA:** ~117

**ANTES:**
```cpp
bool es_alucinacion(const std::string& t) {
    if (t.empty() || t.size() < 4) return true;
    // Repeticion de patron (alucinacion tipica de Whisper)
    std::string tl = t;
    std::transform(tl.begin(), tl.end(), tl.begin(), ::tolower);
    int count = 0;
    size_t pos = 0;
    while ((pos = tl.find("empresa", pos)) != std::string::npos) { ++count; ++pos; }
    if (count >= 2) return true;
    count = 0; pos = 0;
    while ((pos = tl.find("gracias", pos)) != std::string::npos) { ++count; ++pos; }
    if (count >= 2) return true;
    return false;
}
```

**DESPUÉS:**
```cpp
bool es_alucinacion(const std::string& t) {
    if (t.empty() || t.size() < 8) return true;
    // Repeticion de patron (alucinacion tipica de Whisper)
    std::string tl = t;
    std::transform(tl.begin(), tl.end(), tl.begin(), ::tolower);
    int count = 0;
    size_t pos = 0;
    while ((pos = tl.find("empresa", pos)) != std::string::npos) { ++count; ++pos; }
    if (count >= 2) return true;
    count = 0; pos = 0;
    while ((pos = tl.find("gracias", pos)) != std::string::npos) { ++count; ++pos; }
    if (count >= 2) return true;
    if (tl.find('*')        != std::string::npos) return true;
    if (tl.find('[')        != std::string::npos) return true;
    if (tl.find("\xe2\x99\xaa") != std::string::npos) return true;
    if (tl.find("subtitl")  != std::string::npos) return true;
    return false;
}
```

**CAMBIOS:**
- Umbral de tamaño mínimo: 4 → 8 caracteres
- Agregadas 4 nuevas líneas de detección de patrones
- Detecta asteriscos, corchetes, símbolos musicales (♪) y subtítulos

---

## CAMBIO 3: Wake word - eliminar "otto" solo

**ARCHIVO:** src/otto_audio/cpp/otto_pipeline.cpp  
**FUNCIÓN:** `es_wake_word()`  
**LÍNEA:** ~132

**ANTES:**
```cpp
bool es_wake_word(const std::string& t) {
    for (auto& w : {"hola otto","ola otto","hola oto","ola oto","hola auto","otto"})
        if (t.find(w) != std::string::npos) return true;
    return false;
}
```

**DESPUÉS:**
```cpp
bool es_wake_word(const std::string& t) {
    for (auto& w : {"hola otto","ola otto","hola oto","ola oto","hola auto"})
        if (t.find(w) != std::string::npos) return true;
    return false;
}
```

**CAMBIOS:**
- Eliminada la palabra clave "otto" del array de wake words
- Ahora requiere al menos "hola" o "ola" antes de activar

---

## CAMBIO 4: Timeout Ollama - bajar de 120s a 30s

**ARCHIVO:** src/otto_audio/cpp/otto_pipeline.cpp  
**FUNCIÓN:** `ollama_query()`  
**LÍNEA:** ~169

**ANTES:**
```cpp
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return "";

    struct timeval tv{120, 0};
```

**DESPUÉS:**
```cpp
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return "";

    struct timeval tv{30, 0};
```

**CAMBIOS:**
- Timeout de socket: 120 segundos → 30 segundos
- Respuestas más rápidas y mejor UX

---

## CAMBIO 5: CAPTURE_SECS diferente por estado

**ARCHIVO:** src/otto_audio/cpp/otto_pipeline.cpp  
**FUNCIÓN:** `main()` → dentro del `while(running)`  
**LÍNEA:** ~317

**ANTES:**
```cpp
    while (running) {
        auto chunk = tomar_audio(CAPTURE_SECS);
        if (chunk.empty()) { sleep(1); continue; }
```

**DESPUÉS:**
```cpp
    while (running) {
        size_t secs = (estado == ESCUCHANDO) ? 5 : CAPTURE_SECS;
        auto chunk = tomar_audio(secs);
        if (chunk.empty()) { sleep(1); continue; }
```

**CAMBIOS:**
- En estado HIBERNACION: usa CAPTURE_SECS (3 segundos) para detección rápida de wake word
- En estado ESCUCHANDO: usa 5 segundos para captura más larga de preguntas del usuario
- Mejora la detección de wake words sin sacrificar captura de pregunta

---

## CAMBIO 6: Corregir volúmenes en otto_say.sh

**ARCHIVO:** src/otto_audio/scripts/otto_say.sh  
**LÍNEA:** ~13-19

**ANTES:**
```bash
case "${2:-medio}" in
  bajo)  VOL=1.0 ; SDK_VOL=30  ;;
  medio) VOL=2.0 ; SDK_VOL=60  ;;
  alto)  VOL=3.0 ; SDK_VOL=100  ;;
  max)   VOL=5.0 ; SDK_VOL=100 ;;
  *)     VOL=2.0 ; SDK_VOL=70  ;;
esac
```

**DESPUÉS:**
```bash
case "${2:-medio}" in
  bajo)  VOL=1.0 ; SDK_VOL=30  ;;
  medio) VOL=2.5 ; SDK_VOL=60  ;;
  alto)  VOL=6.0 ; SDK_VOL=100 ;;
  max)   VOL=9.0 ; SDK_VOL=100 ;;
  *)     VOL=2.5 ; SDK_VOL=60  ;;
esac
```

**CAMBIOS:**
- `medio`: VOL 2.0 → 2.5 (más claridad)
- `alto`: VOL 3.0 → 6.0 (mucho más audible)
- `max`: VOL 5.0 → 9.0 (máxima potencia)
- Default `*`: VOL 2.0 → 2.5 y SDK_VOL 70 → 60 (consistencia con medio)

---

## Resumen

✅ **6 cambios completados exitosamente**
- 5 cambios en `otto_pipeline.cpp`
- 1 cambio en `otto_say.sh`
- Archivos listos para compilación en robot Unitree G1-EDU
- Sin cambios en nombres de funciones, variables ni structs
- Sin nuevas dependencias o includes
