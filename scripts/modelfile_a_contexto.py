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

ESTE SCRIPT NO MODIFICA EL MODELFILE. El orden seguro es:

  1. python3 scripts/modelfile_a_contexto.py            # ver el corte
  2. python3 scripts/modelfile_a_contexto.py --escribir /tmp/ctx
  3. Cargar los tres en el robot y activarlos:
       for f in /tmp/ctx/*.md; do
         n=$(basename "$f" .md)
         otto_context.sh save "$n" < "$f"
       done
       otto_context.sh set-active general carreras ingreso
     (o pegarlos desde la pestaña Contexto de la web)
  4. **Verificar que Otto siga contestando igual de bien.**
  5. Recién ahí: `--modelfile-corto > Modelfile` y `ollama create`.

Si se invierte el orden 4 y 5, Otto se queda sin saber nada.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

MODELFILE = Path(__file__).resolve().parent.parent / "Modelfile"

# Las secciones de CONOCIMIENTO, agrupadas en los contextos que se van a cargar
# en el robot. El resto del SYSTEM (identidad, cómo habla, ejemplos, humor,
# reglas críticas, preguntas incómodas, restricciones, objetivo) es
# comportamiento y se queda en el Modelfile.
#
# "Preguntas incómodas" se queda del lado del comportamiento a propósito aunque
# parezca información: define la POSTURA de Otto ante una pregunta hostil, no un
# dato que pueda cambiar porque la universidad abrió una carrera nueva.
#
# POR QUÉ AGRUPADO Y NO UN SOLO CONTEXTO
# Todo el conocimiento junto son ~4150 tokens de los 8192 de la ventana, y el
# Modelfile ya se lleva ~2818: casi no queda lugar para conversar. Partido por
# tema se prende sólo lo que el evento necesita. Una visita general no necesita
# el catálogo de posgrados; una feria de ingreso sí necesita el SIA.
GRUPOS: dict[str, list[str]] = {
    # Lo que se pregunta siempre, en cualquier evento.
    "general": [
        "SOBRE UADE",
        "SEDES Y CAMPUS",
        "UADE LABS",
        "UADE HOUSING",
        "CENTRO DEPORTIVO",
        "CANALES DE CONTACTO",
    ],
    # El catálogo. Pesado y sólo hace falta si van a preguntar qué estudiar.
    "carreras": [
        "OFERTA ACADÉMICA — GRADO Y PREGRADO",
        "POSGRADOS — UADE BUSINESS SCHOOL",
    ],
    # Examen, fechas, ranqueo, equivalencias. Para ferias y charlas de ingreso.
    "ingreso": [
        "SISTEMA DE ADMISIÓN (SIA)",
    ],
}

SECCIONES_CONOCIMIENTO = [s for secs in GRUPOS.values() for s in secs]

SECCION_RE = re.compile(r"^===\s*(.+?)\s*===\s*$")


def partir(lineas: list[str]) -> tuple[dict[str, list[str]], list[str]]:
    """Devuelve ({grupo: líneas}, comportamiento).

    Todo lo que está fuera de una sección `=== ... ===` (el encabezado del
    SYSTEM, el cierre con OBJETIVO/RECORDÁ, los PARAMETER) cuenta como
    comportamiento: son las reglas, y perderlas romperían el modelo.
    """
    de_seccion = {s: g for g, secs in GRUPOS.items() for s in secs}
    grupos: dict[str, list[str]] = {g: [] for g in GRUPOS}
    comportamiento: list[str] = []
    destino = comportamiento

    for linea in lineas:
        match = SECCION_RE.match(linea)
        if match:
            grupo = de_seccion.get(match.group(1))
            destino = grupos[grupo] if grupo else comportamiento
        destino.append(linea)

    return grupos, comportamiento


def titulos_presentes(lineas: list[str]) -> list[str]:
    return [m.group(1) for m in (SECCION_RE.match(l) for l in lineas) if m]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--modelfile", type=Path, default=MODELFILE)
    modo = parser.add_mutually_exclusive_group()
    modo.add_argument("--contexto", metavar="NOMBRE", choices=list(GRUPOS),
                      help=f"un grupo a stdout ({', '.join(GRUPOS)})")
    modo.add_argument("--escribir", metavar="DIR", type=Path,
                      help="escribe un .md por grupo en DIR")
    modo.add_argument("--modelfile-corto", action="store_true",
                      help="el Modelfile SIN el conocimiento (ultimo paso)")
    args = parser.parse_args()

    if not args.modelfile.is_file():
        print(f"ERROR: no encuentro {args.modelfile}", file=sys.stderr)
        return 1

    lineas = args.modelfile.read_text(encoding="utf-8").splitlines(keepends=True)
    grupos, comportamiento = partir(lineas)

    # Una sección que se renombró en el Modelfile y ya no matchea se iría en
    # silencio al lado equivocado. Mejor gritar que producir un corte mal hecho.
    presentes = set(titulos_presentes(lineas))
    faltantes = [s for s in SECCIONES_CONOCIMIENTO if s not in presentes]
    if faltantes:
        print("ERROR: estas secciones de conocimiento no estan en el Modelfile.",
              file=sys.stderr)
        print("Si se renombraron, actualiza GRUPOS en este script:", file=sys.stderr)
        for s in faltantes:
            print(f"  - {s}", file=sys.stderr)
        print("\nSecciones encontradas:", file=sys.stderr)
        for s in sorted(presentes):
            print(f"  - {s}", file=sys.stderr)
        return 1

    if args.contexto:
        sys.stdout.write("".join(grupos[args.contexto]))
        return 0

    if args.modelfile_corto:
        sys.stdout.write("".join(comportamiento))
        return 0

    if args.escribir:
        args.escribir.mkdir(parents=True, exist_ok=True)
        for nombre, lns in grupos.items():
            destino = args.escribir / f"{nombre}.md"
            destino.write_text("".join(lns), encoding="utf-8")
            print(f"{destino}  ({sum(len(l) for l in lns)} bytes)")
        return 0

    # Sin flags: el resumen. Es la opcion segura por defecto -- que correrlo sin
    # argumentos no escriba nada ni escupa 14KB a la terminal.
    total = sum(len(l) for l in lineas)
    cb = sum(len(l) for l in comportamiento)
    print(f"Modelfile: {len(lineas)} lineas, {total} bytes\n")
    print("AL CONTEXTO, en grupos (se prende solo lo que el evento necesita):")
    suma = 0
    for nombre, lns in grupos.items():
        b = sum(len(l) for l in lns)
        suma += b
        print(f"\n  {nombre}  ({b} bytes, ~{b//3.5:.0f} tokens)")
        for s in titulos_presentes(lns):
            print(f"      - {s}")
    print(f"\n  TOTAL conocimiento: {suma} bytes, ~{suma//3.5:.0f} tokens"
          f" ({suma*100//total}% del Modelfile)")
    print(f"\nSE QUEDA EN EL MODELFILE ({len(comportamiento)} lineas, {cb} bytes,"
          f" ~{cb//3.5:.0f} tokens, {cb*100//total}%):")
    for s in titulos_presentes(comportamiento):
        print(f"  - {s}")
    print("  - (mas el encabezado del SYSTEM, el cierre y los PARAMETER)")

    # La ventana del modelo es el limite real, y es lo que decide si conviene
    # prender los tres grupos a la vez o no. num_ctx y CONTEXT_MAX_BYTES estan
    # en Modelfile y otto_pipeline.cpp respectivamente.
    print(f"\nVENTANA (num_ctx 8192): Modelfile ~{cb//3.5:.0f} + conocimiento"
          f" ~{suma//3.5:.0f} = ~{(cb+suma)//3.5:.0f} tokens.")
    if suma > 12000:
        print(f"Los tres grupos juntos dan {suma} bytes y otto_pipeline avisa"
              f" arriba de 12000: prender solo los que hagan falta.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
