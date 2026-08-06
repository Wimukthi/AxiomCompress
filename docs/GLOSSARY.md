# Glossary

Terms used across the Axiom documentation, in plain language. Where a term has
a precise on-disk meaning, the exact definition is in
[FORMAT.md](../FORMAT.md); this page is the everyday one.

## Archive terms

**Archive** — one file that holds many files and folders. Axiom's own archive
format is `.axar`; it also reads and writes `.zip`, and reads several formats
it cannot create.

**AXAR** — Axiom's multi-file archive format, the `.axar` file extension.

**AXC** — Axiom's single-stream format, the `.axc` file extension. One
compressed stream with no file names, folders, or metadata. An `.axar` archive
contains one AXC stream per solid block.

**Solid block** — a group of files compressed together as if they were one
long file. Compressing files together finds repetition *between* them, so the
result is smaller than compressing each file separately. The cost is that
reading one file may mean decompressing its whole block. Axiom keeps every
block independently decodable, so it never has to decompress the entire
archive to reach one file.

**Central directory** — the index at the end of an archive listing every file,
where its bytes live, and its checksums. Reading it is how listing an archive
is instant.

**Entry** — one item stored in an archive: a file, a folder, a symbolic link,
or a hard link.

**Self-extracting archive (SFX)** — an archive glued onto a small program, so
the whole thing is a `.exe` that unpacks itself. The recipient doesn't need
Axiom installed.

**Volume** — one piece of an archive that was split for transport, named
`name.part001.axar`, `name.part002.axar`, and so on.

**Snapshot repository** — an archive that keeps several dated versions of the
same folder without storing unchanged data twice.

## Compression terms

**Compression ratio** — original size divided by compressed size. 4.00x means
the archive is a quarter of the original. Bigger is better.

**Level** — a single dial from 1 to 9 trading speed for size. Level 1 is
fastest, level 9 produces the smallest archive, and 5 is the default. The
level affects only compression: an archive made at level 9 decompresses just
as fast as one made at level 1.

**Method** — which compression engine handles the data. Axiom offers its own
adaptive method plus Zstandard, LZMA2, Deflate, and Store.

**Store** — no compression at all. The right choice for data that is already
compressed, such as JPEG photos or MP4 video.

**Dictionary** / **window** — how far back the compressor can look for
repeated data. A 64 MiB window can reuse something it saw 64 MiB ago; a 32 KiB
window cannot. Bigger windows find more repetition and use more memory.

**LZ77** — the family of compression techniques Axiom is built on. The idea is
simple: when the same sequence of bytes appears twice, replace the second copy
with a short note saying "go back this far and copy this many bytes."

**Match** — one of those repeated sequences, described by a *distance* (how far
back) and a *length* (how many bytes).

**Match finder** — the part of the compressor that hunts for matches. Axiom
has three, chosen by level, from a fast one that checks a few places to a
thorough one that searches a sorted tree.

**Parser** — decides which of the available matches to actually use. A greedy
parser takes the longest match it can see right now. An *optimal* parser
(levels 8 and 9) calculates the cheapest path through the whole block, which is
slower but smaller.

**Entropy coding** — the final squeeze. After repetition is removed, common
symbols are written with fewer bits than rare ones. Axiom uses Huffman coding
and rANS.

**Huffman coding** — a classic entropy coder that assigns each symbol a
whole number of bits.

**rANS** — a newer entropy coder that can spend a fractional number of bits per
symbol, so it packs slightly tighter than Huffman at similar speed.

**Filter** / **transform** — a reversible rearrangement applied before
compression to make data more repetitive. Axiom converts jump addresses in
Windows programs to a relative form, and stores smooth numeric data as
differences. Filters are only kept when a trial run shows they actually help.

**Solid** vs **parallel** — with solid compression the whole input is one
stream, which compresses best; splitting it into independent parallel blocks
lets several CPU cores work at once but loses some repetition across the
boundaries. Axiom balances the two automatically.

**Swarm** — the opt-in mode where CPU cores cooperate *inside* one large block
instead of each taking a separate block. Slower per core, better ratio.

## Integrity and protection

**Checksum** — a short value computed from data, used to notice corruption.
If the data changes, the checksum stops matching.

**CRC-32** — a fast 32-bit checksum. Good at catching accidental damage, not
designed to resist deliberate tampering. Axiom stores one per block and one per
file.

**BLAKE3** — a modern cryptographic hash. Slower than CRC-32 but far stronger:
finding two different files with the same BLAKE3 value is not practically
possible. Axiom stores a BLAKE3 digest per file and checks it during `test`.

**Recovery record** — spare data stored inside the archive that can rebuild
damaged parts of it, using Reed-Solomon coding. A 10% record can survive
roughly 10% of the archive being corrupted. It makes the archive bigger and it
is not a substitute for a second copy on separate storage.

**Reed-Solomon** — the error-correcting maths behind recovery records and
recovery volumes. The same family of codes used on CDs and in QR codes.

**Signature** — proof of who created an archive. The archive owner signs with a
secret key; anyone can verify with the matching public key. Verification fails
if the archive was altered after signing.

**Authenticode** — Windows' own code-signing system for `.exe` files. Signing a
self-extracting Axiom archive with Authenticode covers the archive payload as
well as the extractor.

## Encryption

**Argon2id** — the function that turns a password into an encryption key. It is
deliberately slow and memory-hungry, which makes guessing passwords expensive
for an attacker.

**XChaCha20-Poly1305** — the cipher that encrypts each block. It both hides the
data and detects tampering.

**Password slot** — one password that can open the archive. Axiom encrypts the
archive with a random internal key, then wraps that key once per password. This
is why a password can be added, changed, or removed without re-encrypting any
file data.

**Encrypted names** — the option that also encrypts the archive's index, so
file names, sizes, and checksums are hidden. Without it, an encrypted archive
still shows what's inside.

## Windows and file system terms

**NTFS** — the file system Windows uses. It supports several things Axiom
preserves: alternate data streams, security descriptors, junctions, and sparse
files.

**Alternate data stream (ADS)** — extra hidden data attached to a file on NTFS.
Rarely used, occasionally important.

**Sparse file** — a file with large runs of zeros that the file system doesn't
actually store on disk. Axiom records which parts were really allocated so a
restored file stays sparse.

**Symbolic link** / **junction** / **reparse point** — file system entries that
redirect to somewhere else. Axiom stores them as links rather than following
them, and refuses to write extracted files *through* one, which is a common
archive attack.

**Hard link** — two names for the same file content. Axiom stores the content
once and re-links on extraction.

**Per-monitor DPI** — Windows scaling that can differ between monitors. Axiom
redraws its fonts, icons, and layout when a window moves to a different
display.

## Sizes

Axiom follows the IEC convention throughout its interface and documentation:

| Unit | Meaning |
|---|---|
| KiB | 1,024 bytes |
| MiB | 1,024 KiB |
| GiB | 1,024 MiB |
| KB / MB / GB | Decimal thousands. Used only where a benchmark source reports them |
