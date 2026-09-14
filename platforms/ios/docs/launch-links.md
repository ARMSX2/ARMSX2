# ARMSX2 iOS Launch Links

Other apps can start a game in ARMSX2, or read its game list, by opening an
`armsx2://` URL.

## Copy a Link

Long-press a game in the library and tap Copy Launch Link. The link goes to the
clipboard and is shown in an alert.

## Launch a Game

```text
armsx2://launch?game=Shadow%20of%20the%20Colossus.iso
```

- The scheme can be `armsx2`, `armsx2-ios` or `armsx2ios`.
- The verb can be `launch`, `boot` or `play`.
- The parameter can be `game`, `iso`, `file` or `name`.

The value is the filename exactly as ARMSX2 lists it, extension included. It is
not the game's title and not a full path. `.elf` files work too, and so do games
in external folders. When two folders hold the same filename, the first one
found boots.

Percent-encode the filename. Spaces, `&`, `=` and `+` break the link otherwise.

If a game is already running, ARMSX2 asks before it shuts that game down.

## Read the Game List

```text
armsx2://library?callback=yourapp://whatever
```

ARMSX2 opens the callback with `source=armsx2-ios` and `payload`, a base64url
JSON object without padding:

- `schema` is `com.armsx2.library.v1`.
- `app`, `version`, `generatedAt` and `gameCount` describe the export.
- `games` gives `title`, `fileName`, `serial`, `region`, `crc`, `fileSize`,
  `fileType` and `launchURL` for every game, including games in external folders.

`.elf` files are left out of the list.
