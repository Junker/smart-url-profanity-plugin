# smart_url Profanity plugin

A [Profanity](https://profanity.im/) plugin that annotates URLs in incoming messages with short codes and provides commands to open, save, or copy URLs by code — with automatic beautification of long URLs.

## Features

- Rewrites URLs in incoming chat, MUC, and private messages with a short code prefix
- **XEP-0363 aware** — detects HTTP Upload services via service discovery and `aesgcm://` scheme:
  - `📤[kR]upload.example.org/…/report.pdf` (XEP-0363 upload — your server or peer's)
  - `🔗[7p]cdn.external.com/…/image.png` (regular external link)
- **Beautifies long URLs** — short ones are shown in full, long ones are condensed to host + filename
- **Sqids-inspired codes** — offset-based scrambling so consecutive URLs get very different-looking codes
- **Configurable alphabet** — any 3+ unique printable ASCII characters
- Tab-completion for subcommands and URL codes
- Clipboard copy via GTK3 (works on both X11 and Wayland)
- Settings persist across sessions

## Commands

| Command | Description |
|---------|-------------|
| `/surl open <code>` | Open the URL associated with the code (delegates to `/url open`) |
| `/surl save <code>` | Save the URL associated with the code (delegates to `/url save`) |
| `/surl copy <code>` | Copy the full URL to clipboard |
| `/surl short on\|off` | Enable/disable URL annotation in messages (default: on) |
| `/surl maxlen <number>` | Set max URL display length before shortening (10–1000, default: 80) |
| `/surl alphabet <chars>` | Set the code alphabet (3+ unique printable ASCII characters) |
| `/suo <code>` | Shorthand for `/surl open` |
| `/sus <code>` | Shorthand for `/surl save` |
| `/suc <code>` | Shorthand for `/surl copy` |

Tab-completion is available for subcommands, values, and codes:

```
/surl <Tab>            → open, save, copy, short, maxlen, alphabet
/surl short <Tab>      → on, off
/surl open <Tab>       → lists all known codes
/suo <Tab>             → lists all known codes
```

### Settings examples

```
/surl short off          → disable URL annotation (messages pass through unchanged)
/surl short on           → re-enable URL annotation
/surl maxlen             → show current maxlen value
/surl maxlen 80          → only shorten URLs longer than 80 characters
/surl alphabet           → show current alphabet
/surl alphabet abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789
                         → set alphabet (at least 3 unique printable ASCII chars)
```

## Emoji detection

URLs are prefixed with an emoji based on where they point:

| Emoji | Meaning |
|-------|---------|
| 📤 | XEP-0363 HTTP Upload — `aesgcm://` URLs, or URLs matching a disco-discovered upload service, or URLs on a subdomain of your JID domain |
| 🔗 | Regular external link — anything else |

Detection works in layers:

1. `aesgcm://` scheme → always 📤
2. Host matches a disco-discovered upload service → 📤
3. Host is a subdomain of your JID domain (heuristic) → 📤
4. Otherwise → 🔗

## Building

### With clipboard support (default, requires GTK3 dev headers)

```bash
make
```

### Without clipboard support

If you don't have GTK3 development headers installed, or don't need clipboard functionality:

```bash
make WITHOUT_CLIPBOARD=1
```

When built without clipboard support, `/surl copy` and `/suc` will display a message explaining that clipboard support is not compiled in.
This produces `build/smart_url.so`.

## Installation

Install the plugin to your local Profanity plugins directory:

```bash
make install
```

This copies the plugin to `~/.local/share/profanity/plugins/`.

## Usage

Load the plugin in Profanity:

```
/plugins load smart_url
```

Incoming messages with URLs will be automatically annotated:

> 📤[kR]upload.example.org/…/report.pdf  
> 🔗[7p]cdn.external.com/…/image.png

Use the short code to open, save, or copy the URL — the **full** URL is always stored internally:

```
/surl open kR
/sus 7p
/suc kR
```

Settings are persisted automatically and survive restarts.

## Uninstallation

```bash
rm ~/.local/share/profanity/plugins/smart_url.so
```
