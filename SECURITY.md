# Security

## Reporting a vulnerability

Please report security problems privately, through GitHub's
[private vulnerability reporting](https://github.com/valentin-best/pptctl/security/advisories/new)
(the repository's **Security** tab, then **Report a vulnerability**), not in a
public issue. Include a deck or a script that reproduces the problem if you can.
This is a one-person project: expect an answer within a week.

Fixes go into the next release. Only the latest release is supported.

## What pptctl defends against

pptctl reads `.pptx` files it didn't write, often from people it doesn't trust:
the GitHub Action runs it on the decks a pull request brings, and a pre-commit
hook on whatever deck is staged.
A deck is a ZIP of XML and media, so a hostile one can try to exhaust memory or
CPU, or to make pptctl write where it shouldn't. pptctl doesn't render decks and
never runs anything inside them (macros, OLE objects and scripts are only
bytes to it).

- **Decompression bombs.** Before inflating anything, `Package::open()` checks
  the sizes the archive declares. A part of 64 KiB or more may not inflate more
  than 200:1, and all parts together may not inflate past 4 GiB; a file over
  either limit is refused with a message naming the part. Real decks stay far
  below both (the highest ratio over 58 decks from 7 producers is 26:1). A
  part's buffer never grows past its declared size.
- **Image bombs.** Images are decoded only by `optimize`, and not at all above
  2^27 pixels (about 134 megapixels), whatever the file size.
- **Oversized chart data.** Chart series are read up to 65,536 points; the
  point count a chart declares isn't trusted for allocation.
- **XML entities.** The XML parser (pugixml) doesn't process document type
  declarations: no external entities, no entity expansion (no XXE, no
  "billion laughs").
- **Paths.** pptctl writes only to the paths you give it (`-o`, `--in-place`)
  and to a temporary directory of its own; it never creates a file named after
  a part, so a crafted part name can't place files elsewhere.

## Out of scope

- How decks render in PowerPoint, Keynote, LibreOffice or Google Slides, and
  the macros or embedded objects a deck carries: pptctl reports on some of
  these (`lint`) but doesn't sanitize them.
- Running pptctl on decks you trust with more resources than the limits allow:
  the limits are fixed, so a deck over 4 GiB uncompressed can't be read.

## The GitHub Action and pull requests from forks

The Action only reads the checked-out decks and writes to the job summary and
annotations; its `token` input is used only to download the pptctl release.
Run it on `pull_request`, as the examples do, not on `pull_request_target`:
that event gives workflows from forks a token with write access.
