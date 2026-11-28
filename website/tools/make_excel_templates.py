#!/usr/bin/env python3
"""Create sample Hadith and Q&A Excel templates (placeholders only)."""
from pathlib import Path
from openpyxl import Workbook

OUT = Path(__file__).resolve().parents[1] / "docs" / "templates"


def hadith() -> None:
    wb = Workbook()
    ws = wb.active
    ws.title = "Hadith"
    ws.append(["Number", "Title", "Text", "Source"])
    # SAMPLE placeholders — replace with verified content before Publish
    ws.append([
        1,
        "SAMPLE Intention",
        "SAMPLE: Actions are judged by intentions. Replace this with a verified narration.",
        "SAMPLE Bukhari",
    ])
    ws.append([
        2,
        "SAMPLE Kindness",
        "SAMPLE: Speak kindly. Replace this with a verified narration.",
        "SAMPLE Muslim",
    ])
    wb.save(OUT / "hadith-template.xlsx")


def qa() -> None:
    wb = Workbook()
    ws = wb.active
    ws.title = "QA"
    ws.append(["Topic", "Question", "Answer", "Source"])
    ws.append([
        "Wudu",
        "SAMPLE: What is wudu?",
        "SAMPLE: Wudu is ritual washing before prayer. Replace with a verified answer.",
        "SAMPLE note",
    ])
    ws.append([
        "Prayer",
        "SAMPLE: How many daily prayers?",
        "SAMPLE: There are five daily prayers. Replace with a verified answer.",
        "",
    ])
    wb.save(OUT / "qa-template.xlsx")


if __name__ == "__main__":
    OUT.mkdir(parents=True, exist_ok=True)
    hadith()
    qa()
    print("wrote templates in", OUT)
