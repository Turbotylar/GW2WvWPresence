# GW2 WvW Presence

A [Nexus](https://raidcore.gg/Nexus) addon for Guild Wars 2. It shows your character, map and **WvW kill count** in your Discord status while you play.

```
Guild Wars 2
Turbotylar · Firebrand
Eternal Battlegrounds · 327,009 WvW kills
🎮 1:00 elapsed
```

Hover the big image to see your next goal, for example **Next: 500,000 (Realm Avenger II)**. A small icon shows your elite spec.

## Features

- **Total WvW player kills**, read from the Realm Avenger achievements. Every tier (I to X) uses the same kill counter, so your full total shows even after you pass the last tier.
- **Next milestone**: finds the next Realm Avenger tier you haven't reached. It covers every tier from 1 kill to 2,500,000, then counts up in steps of 250,000.
- **Character, elite spec and map**, read live from the game. These update within a few seconds when you change map or character.
- **Elite spec icon** from the official GW2 API.
- **Only uses Discord on your PC.** It talks to the Discord app running on your computer. You don't need a Discord bot, token or developer account.
- **Your API key is encrypted** for your Windows account and stored in `%APPDATA%`, not in the game folder. Sharing your addons folder won't leak it.
- **Updates itself** through Nexus when a new release is published here.
- Every line can be turned off in the options.

## Requirements

- Guild Wars 2 with [Nexus](https://raidcore.gg/Nexus) installed
- The Discord **desktop app** (the browser version can't receive game statuses)
- A GW2 API key with the **account** and **progression** permissions

## Install

1. Download `GW2WvWPresence.dll` from the [latest release](../../releases/latest).
2. Put it in your `Guild Wars 2\addons` folder.
3. Start the game and load **GW2 WvW Presence** in Nexus (default key **Ctrl+O**).
4. Make a GW2 API key at <https://account.arena.net/applications>. Tick **account** and **progression** (the others aren't needed).
5. Open the addon's options in Nexus, paste the key and click **Save key**.

Your status should appear in Discord within a few seconds. The GW2 API only refreshes kill counts every few minutes, so the number can lag a little behind the game.

## Options

| Option | What it does |
| --- | --- |
| Show on Discord | Turns the status on or off. Turning it off clears your status straight away. |
| Show tier name | Adds the Realm Avenger tier name to the hover text. |
| Show play time | Shows how long you've been playing this session. |
| Show character name / profession / map | Hides any of these if you'd rather not share them. |
| Class icon | Shows the small elite spec icon. |
| **Advanced** | Discord application ID, image, and how often the GW2 API is checked (default 60 s). |

## Troubleshooting

The options page shows two status lines, **GW2 API** and **Discord**, saying what's happening.

- **Another status shows instead of this one.** Another addon is also setting your Discord status. Turn off its rich presence feature:
  - **Blish HUD**: Settings → Modules → *Discord Rich Presence* → Disable
  - **Discordant**: turn off its rich presence (the voice overlay can stay on)
- **"Discord desktop app not running."** Start the Discord desktop app. The addon reconnects within a few seconds.
- **"Discord refused access … different permissions."** Discord and Guild Wars 2 are running with different permissions, one as administrator and one not. Run both normally, or both as administrator.
- **"GW2 API error 401/403."** The key is wrong or is missing the *account* or *progression* permission. Make a new key and save it again.
- **No status in Discord at all.** Go to Discord → User Settings → **Activity Privacy** and turn on *Share your detected activities with others*.

## Using your own Discord app (optional)

The status title ("Guild Wars 2") and the big image come from the Discord application the addon uses. To show your own name or picture:

1. Create an application at <https://discord.com/developers/applications>. Its **name** becomes the status title.
2. Under **Rich Presence → Art Assets**, upload a square image named `gw2`.
3. Copy the **Application ID** into the addon's **Advanced** options.

## Privacy

- The addon talks to two places: `api.guildwars2.com`, to read your kills and look up map and spec names, and the Discord app on your own PC.
- Your API key is stored encrypted with Windows DPAPI, so only your Windows account can read it. It's sent to the GW2 API and nowhere else.
- What shows on Discord is exactly what's described above. Turn any part off in the options.

## Building

The addon is one C++ file (`src/presence.cpp`). It needs the Nexus API header and Dear ImGui 1.80, which are in `third_party/`.

```sh
pip install ziglang
./build.sh            # writes build/GW2WvWPresence.dll
```

Zig is used as a cross-compiler, so this works on Linux, macOS or Windows. To publish an update, raise `Def.Version` in `src/presence.cpp`, then create a GitHub release whose tag matches it (for example `v0.2.2`) with the DLL attached. Nexus picks it up on the next game start.

## Credits

Made by Turbotylar. Uses the [Nexus](https://raidcore.gg/Nexus) API header (MIT) and [Dear ImGui](https://github.com/ocornut/imgui) (MIT); their licences are in `third_party/`.

Guild Wars 2 © ArenaNet. This addon isn't affiliated with or endorsed by ArenaNet or Discord.
