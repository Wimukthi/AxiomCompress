# Documentation style

The rules this documentation set is written to. Read this before editing any
`.md` file in the repository, so the next change reads like the last one.

## The three standards we follow

| Standard | What we take from it |
|---|---|
| [Diátaxis](https://diataxis.fr/) | How the set is divided. Every document is a tutorial, a how-to guide, a reference, or an explanation — never a blend of all four |
| [Microsoft Writing Style Guide](https://learn.microsoft.com/style-guide/welcome/) | Voice, tone, capitalization, and terminology. Second person, active voice, sentence case for headings |
| [ISO 24495-1:2023](https://www.iso.org/standard/78907.html) (plain language) | Readability. The reader gets what they need, finds it easily, understands it the first time, and can act on it |

None of these is optional flavour. If a sentence fails plain language, rewrite
it; if a document is trying to be two Diátaxis types at once, split it.

## Which type is this document?

Decide before you write. The type sets the shape.

| Type | Answers | Voice | Examples here |
|---|---|---|---|
| **Tutorial** | "Show me it working" | We walk, you follow | The Quick start in the README |
| **How-to** | "I need to do X" | Imperative, task-first | [GUI_GUIDE.md](GUI_GUIDE.md), [BENCHMARKING.md](BENCHMARKING.md), [INSTALLER.md](INSTALLER.md) |
| **Reference** | "What exactly does this do?" | Neutral, exhaustive, dry | [CLI_GUIDE.md](../CLI_GUIDE.md), [FORMAT.md](../FORMAT.md), [FORMAT_SUPPORT.md](FORMAT_SUPPORT.md) |
| **Explanation** | "Why is it built this way?" | Discursive, argues a case | [ARCHITECTURE.md](../ARCHITECTURE.md), [SFX_ARCHITECTURE.md](SFX_ARCHITECTURE.md) |

Mixing types is the most common failure. A reference page that starts
explaining design rationale has stopped being scannable; an explanation that
starts listing every flag has stopped being readable.

## Writing for someone who isn't a compression engineer

Most people reading about an archiver want to protect files, not to study
LZ77. Assume that reader unless the document's type says otherwise.

**Lead with the outcome, then the mechanism.** Not "The parser runs a bounded
dynamic-programming search over candidates from a binary tree." First: "Levels
8 and 9 try many more ways of encoding the same data and keep the smallest."
Then the mechanism, for the reader who wants it.

**Define a term the first time it appears in a document,** in a half-sentence,
even if it is defined elsewhere. Readers arrive from search engines, not from
the table of contents. Full definitions live in [GLOSSARY.md](GLOSSARY.md).

**Say what it means for the reader.** "Reed-Solomon recovery records" means
nothing on its own. "Spare data that can rebuild a damaged archive, at the cost
of making the file bigger" means something.

**Keep sentences short enough to hold in one breath.** Long sentences are
allowed when the idea genuinely has parts, but three short sentences usually
beat one long one.

**Use the second person for anything the reader does.** "Run `axiomc t` before
you delete the old backup," not "the archive should then be tested."

**Never hide a limitation.** If encryption and recovery records can't be
combined, say so at the point where someone would try it, not in a footnote.

## Mechanics

- **Sentence case for headings.** "Compression levels", not "Compression
  Levels".
- **Wrap prose at 80 columns.** Tables and code blocks may run over.
- **Tables for anything with more than three parallel facts.** Prose for
  reasoning.
- **Code blocks are tagged** with the language, and are copy-pasteable as
  written. PowerShell examples assume PowerShell, not `cmd`.
- **Link to the authoritative document instead of restating it.** One fact,
  one home. When a number must appear twice — the headline benchmark in
  the README and the full table in PERFORMANCE.md — say where it came from.
- **Numbers carry units and a date.** "56.5 MB" is a fact; "much smaller" is
  not. Benchmark figures name the machine and the measurement date.
- **Binary units are IEC.** KiB, MiB, GiB for powers of two. Reserve MB and GB
  for decimal, and only where the source data uses them.
- **Windows paths use backslashes; archive paths use forward slashes.** This
  distinction matters to users, so keep it visible.

## Things to avoid

Marketing adjectives — "powerful", "seamless", "blazing", "robust",
"comprehensive". They tell the reader nothing and date badly.

Hedging that adds no information. "It should be noted that the decoder is
bounded" is longer and weaker than "The decoder is bounded."

Restating a heading as the first sentence beneath it.

Passive constructions that hide who acts. "Archives are validated" — by what,
and when?

Undated performance claims, and undated status words like "currently",
"recently", or "new" without a version attached.

## When behaviour changes

Update the documentation in the same change as the code. Specifically:

1. Change the reference page that owns the fact.
2. Change any how-to guide that walks through it.
3. Add a [CHANGELOG.md](../CHANGELOG.md) entry.
4. If a screenshot shows the old behaviour, retake it.

Screenshots are captured at 100% scale on a dark theme, cropped to the window,
and stored as PNG under `docs/images/`. Retake the whole set when the window
chrome or theme changes, so they don't drift apart visually.
