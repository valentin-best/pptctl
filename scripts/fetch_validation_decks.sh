#!/usr/bin/env bash
# Fetch the real-world .pptx corpus used to validate `pptctl info` (issue #6).
#
# The decks themselves are NOT committed (size + unclear licensing); this script
# is the reproducible record of where they came from. It downloads them into a
# gitignored directory, skipping anything that fails or isn't a real OOXML file.
#
# Usage: scripts/fetch_validation_decks.sh [DEST_DIR]   (default: tests/real_world)
#
# Then validate with: scripts/validate_info.sh [DEST_DIR]
#
# Sources are public lecture decks (.edu) plus a few sample-file hosts, chosen
# for variety in slide count, canvas size, theme, fonts, and media weight.
# Expect some link-rot over time; the validation only needs ~10 to succeed.

set -u

dest="${1:-tests/real_world}"
mkdir -p "$dest"

UA="Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 Safari/537.36"

# url<TAB>output-filename
entries=(
"https://math.mit.edu/classes/18.089/summer2015/Lecture/Lecture1.pptx	edu_mit_math_lecture1.pptx"
"https://sciences.utsa.edu/student-programs/marc/_documents/oral-presentation-instructions.pptx	edu_utsa_oral_presentation.pptx"
"https://www.cs.virginia.edu/~bjc8c/class/cs6456-f19/slides/17-virtualization.pptx	edu_uva_virtualization.pptx"
"https://people.cs.umass.edu/~phillipa/CSE390/DNS.pptx	edu_umass_dns.pptx"
"https://www.bumc.bu.edu/facdev-medicine/files/2012/03/Creative-Teaching-and-Learning-Strategies.pptx	edu_bu_teaching_strategies.pptx"
"https://www.bu.edu/cphs/files/2017/06/Reproducibility-Slides-Mayo.pptx	edu_bu_reproducibility.pptx"
"http://www.csun.edu/~me101/Lecture%20slides/Introduction%20to%20Jira.pptx	edu_csun_intro_jira.pptx"
"https://www.augusta.edu/mcg/residents/residentindifficultywovo.pptx	edu_augusta_resident.pptx"
"https://web.stanford.edu/class/cs345d-01/slides/lecture-01.pptx	edu_stanford_cs345d_lecture01.pptx"
"https://www.cs.cornell.edu/courses/cs5412/2018sp/slides/Lecture-23.pptx	edu_cornell_cs5412_lecture23.pptx"
"https://getsamplefiles.com/download/pptx/sample-1.pptx	sample_getsamplefiles_1.pptx"
"https://getsamplefiles.com/download/pptx/sample-2.pptx	sample_getsamplefiles_2.pptx"
"https://getsamplefiles.com/download/pptx/sample-3.pptx	sample_getsamplefiles_3.pptx"
"https://www.filesampleshub.com/download/document/pptx/sample1.pptx	sample_filesampleshub_1.pptx"
"https://www.filesampleshub.com/download/document/pptx/sample2.pptx	sample_filesampleshub_2.pptx"
)

kept=0
for e in "${entries[@]}"; do
    url="${e%%	*}"
    name="${e##*	}"
    out="$dest/$name"
    if ! curl -sSL --max-time 120 -A "$UA" -o "$out" "$url" 2>/dev/null; then
        echo "skip (download failed): $name"
        rm -f "$out"
        continue
    fi
    # A real .pptx is a ZIP — first two bytes "PK". Reject HTML error pages.
    if [[ "$(head -c2 "$out" | xxd -p 2>/dev/null)" == "504b" ]]; then
        echo "ok: $name"
        kept=$((kept + 1))
    else
        echo "skip (not a .pptx): $name"
        rm -f "$out"
    fi
done

echo
echo "fetched $kept deck(s) into $dest"
if [[ "$kept" -lt 10 ]]; then
    echo "warning: fewer than 10 decks fetched — link-rot likely; add more sources" >&2
    exit 1
fi
