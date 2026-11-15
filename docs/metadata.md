# Application metadata, icons and signatures

What Houseguest shows about a program is information. What it allows is a decision. They are produced by different code, from different evidence, and the first never feeds the second.

## Four kinds of information

| Kind | Examples | Who vouches for it | Used by the policy engine |
| --- | --- | --- | --- |
| Visual identity | the icon, the display name | whoever wrote the installer or the file; anyone can copy another program's | never |
| Declared metadata | company, product, description and version in the executable's version resource; publisher and version in the uninstall record | the author of that file or record, unchecked | never |
| Verified evidence | Authenticode result, certificate chain, SHA-256 of the file, package family | Windows (the certificate chain) or the file system (the content) | yes, through `IdentityResolver` |
| Effective identity | the hash, signer subject or package family a rule or an allowed program is pinned to | the owner, at the moment they allowed it | yes, and only this |

`core` (the policy engine) does not include `InstalledApp`, icons, `FileDetails` or anything from the interface, and no function of the engine takes an icon, a display name, a company string or a description as an argument, so changing any of them cannot change a decision for the same file. What the engine receives for a file is its path, SHA-256, signer subject and package family. The signer that policies compare is still the certificate subject string Windows formats (`trusted_publisher`), unchanged by the readable presentation added here.

## Data model

`InstalledApp` (one library entry) keeps, next to the facts about the installation, where each value came from:

| Field | Meaning | Source recorded |
| --- | --- | --- |
| `name` | what the program is called | `origins.name` |
| `company` | publisher named by the installation record, package or platform | `origins.company` |
| `version` | version named by the same | `origins.version` |
| `description` | the executable's own description (version resource) | executable |
| `icons` | places a picture can be read from, in the order found | `kind` of each |
| `alternatives` | values another source reported that lost to a better-ranked one | `source` of each |
| `path`, `install_dir`, `package_family`, `platform`, `product_id`, `kind`, `scope`, `volume` | identity and placement | `sources`, `evidence` |

Values that are absent stay empty and the interface says which absence it is: *Not declared* (the file has no such field), *Not recorded* (the installation record has none), *Not available* (nothing could be read) and *Could not be calculated* (a read failed). A loading placeholder is shown while details are fetched, an error with a retry when the fetch fails. No value is replaced by a generic string.

## Which source wins

Per field, a source of higher rank replaces a lower one and the loser is kept in `alternatives`. Equal ranks keep the first answer. The ranking is written in `information_rank` in `app_catalog.cpp`:

| Rank | Sources | Why |
| --- | --- | --- |
| 3 | Microsoft Store and Xbox packages, game platforms (Steam, Epic, GOG, Ubisoft Connect, EA) | name the product they distribute |
| 2 | the uninstall record in the registry | written by the product's own installer |
| 1 | everything else | hints |
| fallback | the executable's version resource | describes one file, used only when nothing above has an answer |

Names use their own ranks (`name_rank`: package and platform names above registry names above shortcut and folder names above file names). The company declared inside the executable is not merged into the installation record's publisher: both are shown, under different headings, because they are not the same claim.

## Icons

Discovery records where each picture might be, without opening anything: the `DisplayIcon` of the uninstall record (`file,index`, quoted or not, environment variables expanded), the icon of each shortcut, the logo files named by a package manifest or an Xbox game configuration. The host then chooses at the moment the picture is needed (`host/icon_render.cpp`):

1. Image files an installer declared (`.ico`, `.png`, `.jpg`, `.bmp`, `.gif`). The imaging component reads them, picks the frame closest to the wanted size and rejects damaged files.
2. Icon resources an installer declared (`file,index` inside an executable or library). Read with `PrivateExtractIconsW` as data, nothing is run.
3. The executable's own resources, with the shell's `IShellItemImageFactory` as a second reader.

The first genuine source that is at least as large as requested wins; if none is, the largest wins. A program with no icon at all yields no picture, not the system's generic program icon, and only then does the interface draw initials. Pictures are scaled to a square without changing their proportions and keep their transparency. The size asked for follows the display scale (32 to 256 pixels in fixed steps).

Package logos prefer the unplated variants, which fill their canvas, over the plated ones with a coloured tile.

Robustness: image files over 16 MB, dimensions over 1024 pixels, network paths and relative paths are refused; resource extraction runs under a structured exception handler so a damaged executable cannot take the window down; one worker thread reads, so a slow file delays only pictures. Results are cached in memory and in the user's local data, keyed by the change stamp of every file that could supply the picture, so a replaced icon is read again; "no picture" is cached too. Disk entries older than 45 days are removed at start. Other pages show the same picture as Applications because the library's sources are shared with them by path.

Not implemented: pictures from the stores of platforms that keep them outside the install folder (Steam's artwork cache, Epic's launcher cache). Their programs use the executable's own icon.

## Signatures

`inspect_signature` (system) reads an embedded Authenticode signature and, when there is none, the security catalogs of the system, and returns a state:

| State | Meaning |
| --- | --- |
| `valid` | signature verifies and the certificate chains to a trusted root |
| `untrusted` | a signature exists but its certificate is not trusted (unknown root, broken chain, disallowed by policy) |
| `expired` | a signature exists, its certificate has expired and nothing proves it was signed in time |
| `invalid` | the file changed after signing, or the certificate was revoked or explicitly blocked |
| `unsigned` | no embedded signature and no catalog vouches for the file |
| `unverified` | the check could not complete (unreadable file, unexpected error) |

A certificate is read even when the signature is not valid, so a modified file still names who signed the original; it is reported as `invalid`, not as signed. The name shown to people is the certificate's common name, falling back to its organization. Subject and issuer are decoded attribute by attribute; attributes without a common label (such as the jurisdiction OIDs of extended-validation certificates) keep their OID as a tooltip and never become the name. Thumbprints (SHA-1 and SHA-256), serial, algorithm, validity and whether a trusted timestamp is present are in the advanced view.

Limits: revocation is not checked against the network (`WTD_REVOKE_NONE`, cache-only retrieval), so a certificate revoked very recently can still read as valid, and the interface says so. Verification is a point in time, remembered by file stamp (`FileDetails`, 512 files) and never used for a decision. Enforcement still verifies the file itself.

The pages that show a stored signer (History, blocked-program review, discovery, rules, pending reviews) turn the stored subject into the signer's name with `ui/src/certificate.ts`, keep the full string as a tooltip or copyable value, and never edit the stored text.

## Caches

| Cache | Key | Stale when |
| --- | --- | --- |
| Icon pictures (memory and `icons` folder) | size and every candidate file with its change stamp | any candidate changes; the library epoch moves in the interface |
| Picture promises in the interface | size, path and sources | the catalog generation changes |
| File details (`FileDetails`) | canonical path and file stamp | the stamp changes |
| Library sources (`icons`, origins, alternatives) | catalog version 3 | a catalog refresh; an older catalog file is ignored and rebuilt |

## Measurements

On the development machine (328 library entries with a program path; one local installation), by `scripts/survey-icons.ps1`:

| Measure | Before | After |
| --- | --- | --- |
| Entries for which a picture was produced | 326 | 275 genuine pictures |
| Pictures taken from a declared icon file, shortcut or package asset | 0 | 110 |
| Entries now drawn as initials | 2 | 53 |
| Time to render every entry, no cache | not measured | 0.42 s (1.3 ms per entry on average) |

The 51 entries that went from a picture to initials have no icon resource and no declared icon (command-line tools and similar): before, the window showed Windows' generic program icon for them. VALORANT failed before for two reasons: its installation record writes the install folder with forward slashes, which made the path handed to the shell invalid, and its own icon file named in the record was never read. Both are fixed at the mechanism (paths are normalised where they enter the catalog, and declared icons are collected for every entry), not for that program.

Not measured: icon quality on high-DPI displays other than by inspection of rendered pictures, first-paint time in the real window, behaviour with libraries of several thousand entries.
