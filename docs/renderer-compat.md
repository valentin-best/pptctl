# Renderer compatibility: LibreOffice

`optimize` and `merge` rewrite decks, and every deck they write has to open
without complaint in PowerPoint, Keynote, LibreOffice and Google Slides. Of
those four, LibreOffice is the only one that runs headless in CI, so it gets
a repeatable check: `.github/workflows/renderer-compat.yml` runs
[`scripts/renderer_compat.py`](../scripts/renderer_compat.py) weekly, by hand,
and on pull requests that touch `optimize` or `merge`. This page is its
latest result, regenerated with:

```bash
python3 scripts/renderer_compat.py fetch
python3 scripts/renderer_compat.py run build/macos-release --doc docs/renderer-compat.md
```

Each deck in the corpus below is optimized, and consecutive pairs are merged
twice: once with each deck keeping its own formatting, once with `--theme=1`.
An output passes when:

- **integrity:** `pptctl lint --only integrity` finds no more problems in it
  than in its sources;
- **converts:** LibreOffice writes a PDF of it (LibreOffice can exit 0 without
  writing one, so the PDF is what is checked);
- **pages:** the PDF has as many pages as the PDFs of its sources together.
  The source's PDF is the reference rather than the slide count because
  LibreOffice leaves hidden slides out.

A deck whose source LibreOffice can't convert is listed but not judged.

**What this does not check.** It shows a deck loads and converts, not that it
looks the same: a picture drawn in the wrong place, a lost colour or a
substituted font all still pass. Nor does it see the repair prompt PowerPoint
shows or a deck Keynote refuses. Those checks stay manual: see
[the optimize validation](phase5-optimize-validation.md) and
[the merge validation](phase6-merge-validation.md).

## Latest result

0.6.1, LibreOffice 26.8.2.2 9dea406ab7695df6325b11b0e19fdca8db4b7276, 2026-11-04: **32 pass, 0 fail, 0 not judged** (16 optimized decks, 16 merges).

### optimize

| Deck | Producer | Slides | Size | Integrity findings | PDF pages | Result |
|---|---|---:|---|---|---|---|
| ppt-preprints-report | Microsoft Office PowerPoint | 70 | 1.1 MiB → 928 KiB | 0 → 0 | 70 → 70 | pass |
| ppt-conference-charts | Microsoft Office PowerPoint | 14 | 2.7 MiB → 1.6 MiB | 0 → 0 | 14 → 14 | pass |
| ppt-training-smartart | Microsoft Office PowerPoint | 51 | 2.8 MiB → 1.4 MiB | 0 → 0 | 50 → 50 | pass |
| ppt-figure-odd-size | Microsoft Office PowerPoint | 1 | 2.4 MiB → 2.3 MiB | 0 → 0 | 1 → 1 | pass |
| pptmac-smartart | Microsoft Macintosh PowerPoint | 17 | 1.4 MiB → 1.2 MiB | 0 → 0 | 17 → 17 | pass |
| pptmac-notes | Microsoft Macintosh PowerPoint | 32 | 423 KiB → 394 KiB | 0 → 0 | 32 → 32 | pass |
| gslides-notes | Google Slides | 19 | 884 KiB → 273 KiB | 0 → 0 | 19 → 19 | pass |
| gslides-portrait-cards | Microsoft Office PowerPoint (from Google Slides) | 44 | 4.6 MiB → 1.8 MiB | 0 → 0 | 44 → 44 | pass |
| gslides-three-masters | Google Slides | 14 | 1.4 MiB → 744 KiB | 0 → 0 | 13 → 13 | pass |
| libreoffice-linked-art | LibreOffice/6.0.7.3 | 7 | 1.9 MiB → 992 KiB | 0 → 0 | 7 → 7 | pass |
| libreoffice-workshop | LibreOffice/25.2.7.2 | 24 | 3.7 MiB → 2.8 MiB | 0 → 0 | 24 → 24 | pass |
| libreoffice-from-gslides | LibreOffice/7.6.0.3 (from Google Slides) | 13 | 223 KiB → 164 KiB | 0 → 0 | 13 → 13 | pass |
| onlyoffice-hardware | onlyoffice/7.3.3.49 | 13 | 3.0 MiB → 2.3 MiB | 0 → 0 | 13 → 13 | pass |
| keynote-charts | Keynote 14.5 (export) | 2 | 39 KiB → 28 KiB | 0 → 0 | 2 → 2 | pass |
| python-pptx-notes-masters | python-pptx 1.0.2 | 4 | 43 KiB → 23 KiB | 0 → 0 | 4 → 4 | pass |
| python-pptx-comments | python-pptx 1.0.2 | 2 | 30 KiB → 15 KiB | 0 → 0 | 2 → 2 | pass |

### merge

| Decks | Formatting | Integrity findings | PDF pages | Result |
|---|---|---|---|---|
| ppt-preprints-report + ppt-conference-charts | each keeps its own | 0 → 0 | 84 → 84 | pass |
| ppt-preprints-report + ppt-conference-charts | `--theme=1` | 0 → 0 | 84 → 84 | pass |
| ppt-training-smartart + ppt-figure-odd-size | each keeps its own | 0 → 0 | 51 → 51 | pass |
| ppt-training-smartart + ppt-figure-odd-size | `--theme=1` | 0 → 0 | 51 → 51 | pass |
| pptmac-smartart + pptmac-notes | each keeps its own | 0 → 0 | 49 → 49 | pass |
| pptmac-smartart + pptmac-notes | `--theme=1` | 0 → 0 | 49 → 49 | pass |
| gslides-notes + gslides-portrait-cards | each keeps its own | 0 → 0 | 63 → 63 | pass |
| gslides-notes + gslides-portrait-cards | `--theme=1` | 0 → 0 | 63 → 63 | pass |
| gslides-three-masters + libreoffice-linked-art | each keeps its own | 0 → 0 | 20 → 20 | pass |
| gslides-three-masters + libreoffice-linked-art | `--theme=1` | 0 → 0 | 20 → 20 | pass |
| libreoffice-workshop + libreoffice-from-gslides | each keeps its own | 0 → 0 | 37 → 37 | pass |
| libreoffice-workshop + libreoffice-from-gslides | `--theme=1` | 0 → 0 | 37 → 37 | pass |
| onlyoffice-hardware + keynote-charts | each keeps its own | 0 → 0 | 15 → 15 | pass |
| onlyoffice-hardware + keynote-charts | `--theme=1` | 0 → 0 | 15 → 15 | pass |
| python-pptx-notes-masters + python-pptx-comments | each keeps its own | 0 → 0 | 6 → 6 | pass |
| python-pptx-notes-masters + python-pptx-comments | `--theme=1` | 0 → 0 | 6 → 6 | pass |

## Corpus

`tests/corpus/manifest.json` lists the decks with their download URL and
SHA-256; `fetch` verifies every download. The decks from Zenodo are not
committed. The Keynote export and the python-pptx fixtures are in the
repository.

| Deck | Producer | Licence | Source |
|---|---|---|---|
| ppt-preprints-report | Microsoft Office PowerPoint | CC-BY-4.0 | [Practices, drivers and impediments in the use of preprints (Phase 1 report)](https://doi.org/10.5281/zenodo.2654832), Chiarelli, Andrea; Johnson, Rob; Pinfield, Stephen; Richens, Emma |
| ppt-conference-charts | Microsoft Office PowerPoint | CC-BY-4.0 | [2nd Basque Conference on Cyber Physical Systems and Artificial Intelligence 2023](https://doi.org/10.5281/zenodo.10037767), Manuel Graña |
| ppt-training-smartart | Microsoft Office PowerPoint | CC-BY-SA-4.0 | [Materialien zur Zusatzausbildung "Referenzperson für schulisches Handeln im Kontext sexuellen Kindesmissbrauchs" (RP SKM)](https://doi.org/10.5281/zenodo.17091645), Bayer, Lennart; Cigelski, Maike; Eilfgang, Justine; Mensing, Frieda; Pülschen, Simone |
| ppt-figure-odd-size | Microsoft Office PowerPoint | CC-BY-4.0 | [S-allylmercaptocysteine improves alcoholic liver disease partly through a direct modulation of insulin receptor signaling](https://doi.org/10.5281/zenodo.3676958), Luo Pingping; Zheng Ming; Zhang Rui; Zhang Hong; Liu Yingxia; Li Wei; Sun Xiaoming; Yu Qian; Tipoe George Lim; Xiao Jia |
| pptmac-smartart | Microsoft Macintosh PowerPoint | CC-BY-4.0 | [Definieren, identifizieren, fördern. Fair-OA-Aktivitäten monitoren](https://doi.org/10.5281/zenodo.13711901), Voigt, Michaela |
| pptmac-notes | Microsoft Macintosh PowerPoint | CC-BY-4.0 | [DPN Digital Preservation Curriculum](https://doi.org/10.5281/zenodo.1183284), AVPreserve |
| gslides-notes | Google Slides | CC-BY-4.0 | [A first Introduction to Open Science](https://doi.org/10.5281/zenodo.4529129), Melanie Imming; Anke Versteeg; Sander Bosch; Jeroen Sondervan; Carlos Martinez-Ortiz Netherlands eScience Center ;; Loek Brinkman; Esther Plomp; Maria Cruz; Ingeborg Verheul |
| gslides-portrait-cards | Microsoft Office PowerPoint (from Google Slides) | CC-BY-4.0 | [Fresque des données de recherche](https://doi.org/10.5281/zenodo.18834925), León y Barella, Alicia; Larre-Perez, Margaux; Michel-Rodriguez, Monica |
| gslides-three-masters | Google Slides | CC-BY-4.0 | [Introduction to  Semantic Artefacts](https://doi.org/10.5281/zenodo.3549375), Coen, Gerard |
| libreoffice-linked-art | LibreOffice/6.0.7.3 | CC-BY-4.0 | [Linked Open Greek Pottery:  Kerameikos.org and Linked Art](https://doi.org/10.5281/zenodo.3463596), Gruber, Ethan |
| libreoffice-workshop | LibreOffice/25.2.7.2 | CC-BY-4.0 | [Workshop "Naming Files and Folders - Best Practices at the Microscope" from CRC TRR 384 IN-CODE](https://doi.org/10.5281/zenodo.19680929), Lampe, André; Giuliani, Claudia; Binder, Harald; Schmoranzer, Jan |
| libreoffice-from-gslides | LibreOffice/7.6.0.3 (from Google Slides) | CC-BY-4.0 | [Gioacchino Volpe nello specchio del suo archivio](https://doi.org/10.5281/zenodo.13926610), Grilli Lorenzo |
| onlyoffice-hardware | onlyoffice/7.3.3.49 | CC-BY-4.0 | [OSI² Build Workshop 22.09.2023 \| Slides](https://doi.org/10.5281/zenodo.10079661), Winter, Lukas; O'Reilly, Tom; de Gans, Danny; Pellicer-Guridi, Ruben; Littin, Sebastian; Teeuwisse, Wouter; Prier, Marcus; Schote, David; Frintz, Jan; Schüncke, Patrick; Häuer, Martin |
| keynote-charts | Keynote 14.5 (export) | MIT (this repository) | tests/fixtures/charts_deck.pptx exported by Keynote 14.5 (`tests/corpus/keynote_charts.pptx`) |
| python-pptx-notes-masters | python-pptx 1.0.2 | MIT (this repository) | Synthetic fixture: speaker notes and a second slide master (`tests/fixtures/notes_and_masters.pptx`) |
| python-pptx-comments | python-pptx 1.0.2 | MIT (this repository) | Synthetic fixture: legacy comments (`tests/fixtures/merge_comments_a.pptx`) |
