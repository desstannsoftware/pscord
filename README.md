<p align="center">
  <img src="docs/cover.webp" alt="PSCord: an unofficial Discord client for jailbroken PS5">
</p>

# PSCord for PS5

A native Discord client for the jailbroken PS5. It is a standalone homebrew app, drawn with OpenGL and built around the controller.

Download it from [desstannsoftware.com](https://desstannsoftware.com/#pscord).

> PSCord is unofficial and is not affiliated with Discord Inc. Discord's terms do not allow third-party clients, so use it at your own risk.

<p align="center">
  <img src="docs/features.webp" alt="Servers and DMs, rich messages, voice channels, QR sign-in, built-in unblock, self-updating">
</p>

## Features

- **Servers and DMs**: server rail, channels with unread and mention badges, member list with role colours, direct messages.
- **Messages drawn like the desktop client**: markdown, mentions, replies, reactions, embeds, GIFs, stickers, custom emoji, attachments and avatars.
- **Write messages** with an on-screen keyboard (Square).
- **Voice channels**: join a call, hear everyone and talk with a headset mic. Call screen with a tile per person and speaking and muted marks, R2 to mute, Triangle to leave. Discord's end-to-end encrypted (DAVE) calls are supported.
- **Sign in from your phone**: scan the QR code with the Discord mobile app. The token stays on the console.
- **Built-in unblock**: works around DNS and SNI-based Discord blocking inside the app (split TLS handshake, unfiltered DNS). No proxy and no system changes on the console.
- **Self-updating**: on launch it checks for a new version, verifies the download with SHA-256 and restarts into the new version.
- **Coming back to a call**: if the console suspended PSCord, it rejoins the voice channel when Discord reconnects, and mentions and direct messages raise a system notification.
- **Community**: every few launches, and from Settings, PSCord offers [Desstann Homebrew](https://discord.gg/wfTch3qUa6).

## Screenshots

<p align="center">
  <img src="docs/chat.webp" width="49%" alt="A server channel with the member list">
  <img src="docs/voice-call.webp" width="49%" alt="The call screen">
</p>

These are rendered from the app's own interface code with sample data (made-up users and messages). The interface is available in English and Turkish, switchable in the settings.

## Install

1. Download `PPSA99190.zip` from [desstannsoftware.com](https://desstannsoftware.com/#pscord).
2. Extract it and copy the `PPSA99190` folder to `/data/homebrew/`.
3. **PSCord** appears on the home screen, like the other apps in that folder.

## Support

Questions and bug reports: [Desstann Homebrew Discord](https://discord.gg/wfTch3qUa6)

PSCord is free. If it is useful, you can [buy me a coffee](https://buymeacoffee.com/desstann).

<img src="docs/buymeacoffee-qr.png" width="160" alt="QR code for buymeacoffee.com/desstann">
