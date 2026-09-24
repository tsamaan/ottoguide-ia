#!/usr/bin/env python3
"""Separa el Modelfile en dos: lo que Otto SABE y cómo HABLA.

POR QUÉ
El `SYSTEM` del Modelfile mezcla dos cosas de naturaleza distinta:

  - **Comportamiento**: quién es Otto, cómo habla, qué largo tienen las
    respuestas, qué no hace nunca. Cambia poco y es de quien programa el robot.
  - **Conocimiento**: sedes, carreras, aranceles, contactos. Cambia solo (la
    universidad abre una carrera, se muda una sede, cambia un mail) y lo sabe
    gente que no toca código.

Hoy cambiar un dato exige editar el Modelfile y correr `ollama create` en el
robot. Con `contexts/` el conocimiento pasa a ser un archivo editable desde la
web (ver ottohabla/scripts/otto_context.sh), y el Modelfile se queda sólo con el
comportamiento. Este script hace el corte.

CÓMO
Las secciones se identifican por su título `=== ... ===`, no por número de
línea: los números se desactualizan al primer cambio del Modelfile.

ESTE SCRIPT NO MODIFICA NADA. Escribe a stdout. El orden seguro es:

  1. python3 scripts/modelfile_a_contexto.py > /tmp/uade.md
  2. Cargar ese texto como contexto en el robot (pestaña Contexto de la web, o
     `otto_context.sh save uade-general < /tmp/uade.md`) y activarlo.
  3. **Verificar que Otto siga contestando bien.**
  4. Recién ahí: `--modelfile-corto > Modelfile` y `ollama create`.

Si se invierte el orden 3 y 4, Otto se queda sin saber nada.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

MODELFILE = Path(__file__).resolve().parent.parent / "Modelfile"

# Secciones que son CONOCIMIENTO y se van al contexto. El resto del SYSTEM
# (identidad, cómo habla, ejemplos, humor, reglas críticas, preguntas
# incómodas, restricciones, objetivo) es comportamiento y se queda.
#
# "Preguntas incómodas" se queda a propósito aunque parezca información: define
# la POSTURA de Otto ante una pregunta hostil, no un dato que pueda cambiar
# porque la universidad abrió una carrera nueva.
SECCIONES_CONOCIMIENTO = [
    "SOBRE UADE",
    "SEDES Y CAMPUS",
    "UADE LABS",
    "UADE HOUSING",
    "CENTRO DEPORTIVO",
    "OFERTA ACADÉMICA — GRADO Y PREGRADO",
    "POSGRADOS — UADE BUSINESS SCHOOL",
    "SISTEMA DE ADMISIÓN (SIA)",
    "CANALES DE CONTACTO",
]

SECCION_RE = re.compile(r"^===\s*(.+?)\s*===\s*$")


def partir(lineas: list[str]) -> tuple[list[str], list[str]]:
    """Devuelve (conocimiento, comportamiento) como listas de líneas.

    Todo lo que está fuera de una sección `=== ... ===` (el encabezado del
    SYSTEM, el cierre con OBJETIVO/RECORDÁ, los PARAMETER) cuenta como
    comportamiento: son las reglas, y perderlas romperían el modelo.
    """
    conocimiento: list[str] = []
    comportamiento: list[str] = []
    destino = comportamiento

    for linea in lineas:
        match = SECCION_RE.match(linea)
        if match:
            titulo = match.group(1)
            destino = conocimiento if titulo in SECCIONES_CONOCIMIENTO else comportamiento
        destino.append(linea)

    return conocimiento, comportamiento


def titulos_presentes(lineas: list[str]) -> list[str]:
    return [m.group(1) for m in (SECCION_RE.match(l) for l in lineas) if m]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--modelfile", type=Path, default=MODELFILE)
    grupo = parser.add_mutually_exclusive_group()
    grupo.add_argument("--modelfile-corto", action="store_true",
                       help="escribe el Modelfile SIN el conocimiento (paso 4)")
    grupo.add_argument("--resumen", action="store_true",
                       help="qué se lleva, qué se queda y cuánto pesa cada parte")
    args = parser.parse_args()

    if not args.modelfile.is_file():
        print(f"ERROR: no encuentro {args.modelfile}", file=sys.stderr)
        return 1

    lineas = args.modelfile.read_text(encoding="utf-8").splitlines(keepends=True)
    conocimiento, comportamiento = partir(lineas)

    # Una sección que se renombró en el Modelfile y ya no matchea se iría en
    # silencio al lado equivocado. Mejor gritar que producir un corte mal hecho.
    presentes = set(titulos_presentes(lineas))
    faltantes = [s for s in SECCIONES_CONOCIMIENTO if s not in presentes]
    if faltantes:
        print("ERROR: estas secciones de conocimiento no estan en el Modelfile.",
              file=sys.stderr)
        print("Si se renombraron, actualiza SECCIONES_CONOCIMIENTO en este script:",
              file=sys.stderr)
        for s in faltantes:
            print(f"  - {s}", file=sys.stderr)
        print("\nSecciones encontradas:", file=sys.stderr)
        for s in sorted(presentes):
            print(f"  - {s}", file=sys.stderr)
        return 1

    if args.resumen:
        total = sum(len(l) for l in lineas)
        kb = sum(len(l) for l in conocimiento)
        cb = sum(len(l) for l in comportamiento)
        print(f"Modelfile: {len(lineas)} lineas, {total} bytes\n")
        print(f"AL CONTEXTO ({len(conocimiento)} lineas, {kb} bytes, {kb*100//total}%):")
        for s in titulos_presentes(conocimiento):
            print(f"  - {s}")
        print(f"\nSE QUEDA EN EL MODELFILE ({len(comportamiento)} lineas, {cb} bytes, {cb*100//total}%):")
        for s in titulos_presentes(comportamiento):
            print(f"  - {s}")
        print("  - (mas el encabezado del SYSTEM, el cierre y los PARAMETER)")
        if kb > 8000:
            print(f"\nOJO: el contexto daria {kb} bytes y otto_pipeline avisa arriba de 8000.")
            print("Conviene partirlo en varios contextos, o recortar lo que no se pregunta nunca.")
        return 0

    sys.stdout.write("".join(comportamiento if args.modelfile_corto else conocimiento))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
