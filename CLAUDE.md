# Agent Browser Protocol - Chromium Fork

A Chromium fork implementing the Agent Browser Protocol (ABP) - a REST-based API for AI agent browser control at the C++ engine level. Unlike CDP or browser extensions, ABP operates directly in the browser engine for lower latency and greater capability.

## Current Implementation Status

### Working Features
- **Tab Management**: List, create, close, get info
- **Navigation**: Navigate to URL, back, forward, reload
- **Screenshots**: Capture viewport with optional element markup overlays
- **Input**: Click at coordinates, type text (via CDP Input.dispatch*)
- **JavaScript Execution**: Execute scripts and get results (via CDP Runtime.evaluate)

### Architecture

```
┌─────────────────────────────────────────────┐
│              HTTP Client (curl/agent)        │
└─────────────────┬───────────────────────────┘
                  │ GET/POST /api/v1/*
                  ▼
┌─────────────────────────────────────────────┐
│  AbpHttpServer (IO thread)                  │
│  - net::HttpServer on localhost:8222        │
│  - Routes requests, sends JSON responses    │
└─────────────────┬───────────────────────────┘
                  │ PostTask to UI thread
                  ▼
┌─────────────────────────────────────────────┐
│  AbpController (UI thread)                  │
│  - Direct access to Browser, TabStripModel  │
│  - Uses DevToolsAgentHost for CDP commands  │
└─────────────────────────────────────────────┘
```

## Project Structure

### ABP Source Code

```
chrome/browser/abp/
├── BUILD.gn              # Build configuration
├── abp_switches.h/cc     # --enable-abp, --abp-port flags
├── abp_http_server.h/cc  # HTTP server (IO thread)
└── abp_controller.h/cc   # Request handler + CDP client (UI thread)
```

### MCP Server

```
tools/abp-mcp-server/
├── package.json          # Node.js package
├── tsconfig.json         # TypeScript config
└── src/index.ts          # MCP server bridging to ABP REST API
```

### Design Documentation

```
plans/
├── README.md                    # Overview
├── agent-browser-protocol.md    # Core ABP architecture
├── API.md                       # Full REST API specification
├── mcp.md                       # MCP server specification
└── implementation.md            # Minimal implementation plan
```

## Build Setup

### Prerequisites

1. **depot_tools** (Google's build toolchain):
   ```bash
   git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git ~/depot_tools
   echo 'export PATH="$HOME/depot_tools:$PATH"' >> ~/.bashrc
   source ~/.bashrc
   ```

2. **Build dependencies** (Ubuntu/Debian):
   ```bash
   sudo ./build/install-build-deps.sh --no-prompt
   ```

### Directory Structure

```
/home/paladin/src/
├── .gclient           # gclient configuration
├── src -> chromium    # symlink required by gclient
└── chromium/          # source code
    ├── out/Default/   # build output
    ├── chrome/browser/abp/  # ABP implementation
    ├── tools/abp-mcp-server/  # MCP server
    └── plans/         # Design docs
```

### Sync Dependencies

```bash
cd /home/paladin/src
gclient sync --no-history
```

### Configure Build

Debug component build (faster incremental builds):
```bash
cd /home/paladin/src/src
gn gen out/Default --args='is_debug=true is_component_build=true symbol_level=1 dcheck_always_on=true'
```

### Build Chromium

```bash
cd /home/paladin/src/src
autoninja -C out/Default chrome
```

First build: ~4-6 hours. Incremental builds: seconds to minutes.

## Running ABP

### Start Chrome with ABP Enabled

```bash
./out/Default/chrome --enable-abp
```

### REST API Examples

```bash
# List all tabs
curl http://localhost:8222/api/v1/tabs

# Create new tab
curl -X POST http://localhost:8222/api/v1/tabs \
  -H "Content-Type: application/json" \
  -d '{"url":"https://example.com"}'

# Navigate existing tab
curl -X POST http://localhost:8222/api/v1/tabs/{tab_id}/navigate \
  -H "Content-Type: application/json" \
  -d '{"url":"https://example.com"}'

# Take screenshot with element markup
curl -X POST http://localhost:8222/api/v1/tabs/{tab_id}/screenshot \
  -H "Content-Type: application/json" \
  -d '{"screenshot":{"markup":"interactive","format":"webp"}}'

# Click at coordinates
curl -X POST http://localhost:8222/api/v1/tabs/{tab_id}/click \
  -H "Content-Type: application/json" \
  -d '{"x":100,"y":200}'

# Type text
curl -X POST http://localhost:8222/api/v1/tabs/{tab_id}/type \
  -H "Content-Type: application/json" \
  -d '{"text":"hello world"}'

# Execute JavaScript
curl -X POST http://localhost:8222/api/v1/tabs/{tab_id}/execute \
  -H "Content-Type: application/json" \
  -d '{"script":"document.title"}'

# Close tab
curl -X DELETE http://localhost:8222/api/v1/tabs/{tab_id}
```

### MCP Server

The MCP server bridges AI agents (like Claude) to the ABP REST API:

```bash
# Build and run MCP server
cd tools/abp-mcp-server
npm install
npm run build
npm start
```

Configure in Claude Desktop (`claude_desktop_config.json`):
```json
{
  "mcpServers": {
    "browser": {
      "command": "node",
      "args": ["/path/to/chromium/tools/abp-mcp-server/dist/index.js"],
      "env": {
        "ABP_URL": "http://localhost:8222"
      }
    }
  }
}
```

## API Reference

See `plans/API.md` for the complete REST API specification. Key endpoints:

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/v1/tabs` | List all tabs |
| GET | `/api/v1/tabs/{id}` | Get tab details |
| POST | `/api/v1/tabs` | Create new tab |
| DELETE | `/api/v1/tabs/{id}` | Close tab |
| POST | `/api/v1/tabs/{id}/navigate` | Navigate to URL |
| POST | `/api/v1/tabs/{id}/reload` | Reload page |
| POST | `/api/v1/tabs/{id}/back` | Go back |
| POST | `/api/v1/tabs/{id}/forward` | Go forward |
| POST | `/api/v1/tabs/{id}/screenshot` | Capture screenshot |
| POST | `/api/v1/tabs/{id}/execute` | Execute JavaScript |
| POST | `/api/v1/tabs/{id}/click` | Click at coordinates |
| POST | `/api/v1/tabs/{id}/type` | Type text |

## Development Notes

- Source is at `/home/paladin/src/chromium` (symlinked as `src` for gclient)
- Build output at `out/Default/`
- Use `autoninja` (not `ninja`) for automatic parallelism
- Run `gclient sync` after pulling changes to update dependencies
- ABP uses CDP (Chrome DevTools Protocol) internally for screenshots, input, and JS execution
- Tab IDs are DevToolsAgentHost IDs (stable for the session)
