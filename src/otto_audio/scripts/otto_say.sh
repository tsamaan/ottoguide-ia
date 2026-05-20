#!/bin/bash
# @TASK: Texto → Piper TTS → WAV → parlante del robot
# @INPUT: $1 = texto, $2 = volumen (bajo|medio|alto|max) default: medio
# @USAGE: ./otto_say.sh "texto" [bajo|medio|alto|max]

PIPER=~/piper/piper
VOICE=~/piper/voices/es_MX-gevy-high.onnx
SPEAK=~/Desktop/teo_Ottoguide_IA/ottoguide-ia/src/otto_audio/cpp/build/otto_speak_file
IFACE=eth0
TMP_RAW=/tmp/otto_say_raw.wav
TMP_OUT=/tmp/otto_say.wav

if [ -z "$1" ]; then
  echo "Uso: otto_say.sh \"texto\" [bajo|medio|alto|max]"
  exit 1
fi

case "${2:-medio}" in
  bajo)  VOL=1.0 ; SDK_VOL=30  ;;
  medio) VOL=2.5 ; SDK_VOL=60  ;;
  alto)  VOL=6.0 ; SDK_VOL=100 ;;
  max)   VOL=9.0 ; SDK_VOL=100 ;;
  *)     VOL=2.5 ; SDK_VOL=60  ;;
esac

echo "$1" | "$PIPER" --model "$VOICE" --output_file "$TMP_RAW" && \
ffmpeg -y -i "$TMP_RAW" -ar 16000 -ac 1 -sample_fmt s16 -af "volume=${VOL}" "$TMP_OUT" -loglevel quiet && \
"$SPEAK" "$IFACE" "$TMP_OUT" "$SDK_VOL"
