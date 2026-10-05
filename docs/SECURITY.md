# Security model and limits

What Axiom promises when it opens an archive that someone else made, and where
those promises end. Read this before you extract an archive from a source you
don't control. The commands and options named here are in
[CLI_GUIDE.md](CLI_GUIDE.md).

## What extraction promises

Axiom treats every archive as hostile input. These rules hold on every
platform and for AXAR and ZIP archives alike, unless a rule says otherwise.

- **Nothing lands outside the destination.** An entry whose path is absolute,
  climbs out with `..`, names a drive, or contains a NUL character is refused.
- **Nothing is written through a link.** A link is a symbolic link or, on
  Windows, a junction. No folder between the destination and an entry may be
  one, including a link that the same extraction created a moment earlier.
  Axiom writes each file to a randomly named temporary file that it creates new
  beside the destination, then renames it into place, so a link that already
  sits at the temporary name is never used. The links an archive holds are
  created last, after every file and folder.
- **A hard link shares only what this run wrote.** If the file it should point
  to was skipped or left out of the selection, the link gets its own copy of the
  archive's bytes instead of becoming another name for a file that was already
  there. AXAR only.
- **A link's own metadata stays on the link.** Permissions, times, and security
  descriptors are applied to a link only where that can be done without
  following it, and are skipped otherwise. What kind of thing a path is comes
  from the file system, never from a mode stored in the archive.
- **Privilege isn't restored by default.** Set-user-ID and set-group-ID bits
  are left off, and so are the extended attributes in the `security.`,
  `trusted.` and `system.` namespaces on Linux, which hold file capabilities.
  Axiom reports each file whose extended attributes were left out. Pass
  `--restore-privileged` to restore them from an archive you trust.
- **Time and memory are bounded.** For AXAR and AXC, `--max-output` caps the
  size one block may declare it decodes to, and the format caps how much work a
  password-protected archive can ask for when Axiom derives its key.

`axiomc t` applies the same rules to the entry list. It fails an archive that
extraction would refuse, and warns about a path that more than one entry uses.

## What writing promises

- **A new archive appears whole or not at all.** Axiom writes to a temporary
  file beside the destination, whose name has 64 random bits in it, and renames
  it into place. Nothing can plant a link at that name beforehand, and a
  temporary left behind by a killed run is never reused.
- **Replacing an archive is crash-safe.** When the destination already exists,
  Axiom flushes the new contents to disk before the rename that retires the old
  file, so a crash or power loss cannot leave a truncated archive where the good
  one was.
- **Secret keys are private.** `axiomc keygen` creates the secret key readable
  by its owner alone, never overwrites a file that exists, and wipes the key
  from memory afterwards.

## What no archiver can promise

- **A valid archive can still be enormous.** Runs of zeros compress by factors
  in the millions, so a few kilobytes can honestly expand to gigabytes. Set
  `--max-output`, and extract where you have room to spare.
- **Extracted files are still files.** Axiom doesn't scan what it extracts. A
  program you unpack is as safe to run as it was before it was packed.

## Windows limits

These are gaps in what Axiom does on Windows, or behaviour that its tests
cover only where noted.

- **The Mark of the Web isn't passed on.** Windows marks a file you download as
  coming from the internet, which SmartScreen and Office's Protected View read.
  Axiom stores and restores the alternate data streams of the files it archives,
  so a file that carried the mark when it was archived carries it again. It does
  not copy the mark of the archive itself onto the files it extracts from it.
  Some other tools, including Windows Explorer's built-in ZIP extraction, do.
- **Links and junctions are restored as stored.** Like any archiver, Axiom
  recreates the symbolic links and junctions an archive holds, and they can
  point anywhere. Extraction never writes through them, but you can open a file
  through one afterwards. Extract an archive from someone else into a new,
  empty folder. Creating a symbolic link needs Developer Mode or an elevated
  process; without either, Axiom reports a warning and carries on.
- **`axiomc` takes its arguments in the system code page.** A file name that has
  characters outside that code page can't be typed on the command line. Use the
  Axiom app, or the short name that `dir /x` shows, for those files.
- **The 7-Zip library is loaded from Axiom's own folder.** The providers for
  7z, RAR, ISO and CAB load `7z.dll` from `backends\7zip` beside the program, by
  full path. Keep that folder writable only by administrators, which is the
  default under `Program Files`. A portable copy is as safe as the folder you
  put it in.
- **The updater's digest doesn't prove who made the installer.** The update
  check reads the latest release from GitHub over HTTPS, never follows a
  redirect to plain HTTP, and accepts only an installer asset with Axiom's name
  that publishes a SHA-256 digest. It verifies the download against that digest
  before writing it, and again just before it asks Windows to run it. The digest
  comes from the same release as the file, so it catches a damaged or swapped
  download but not a release that someone with access to the repository
  replaced. The updater doesn't check an Authenticode signature, and the
  release workflow doesn't sign the installer or the programs. To check a
  download yourself, compare its SHA-256 with the one on the release page.
