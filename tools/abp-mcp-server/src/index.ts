#!/usr/bin/env node
/**
 * ABP MCP Server - Model Context Protocol server for Agent Browser Protocol
 *
 * Exposes browser control capabilities through MCP tools, bridging
 * MCP-compatible AI systems to the ABP REST API.
 */

import { Server } from "@modelcontextprotocol/sdk/server/index.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import {
  CallToolRequestSchema,
  ListToolsRequestSchema,
  ListResourcesRequestSchema,
  ReadResourceRequestSchema,
  Tool,
} from "@modelcontextprotocol/sdk/types.js";

// Configuration
const ABP_URL = process.env.ABP_URL || "http://localhost:8222";
const ABP_AUTH_TOKEN = process.env.ABP_AUTH_TOKEN;

// HTTP client for ABP REST API
async function abpRequest(
  method: string,
  path: string,
  body?: unknown
): Promise<unknown> {
  const url = `${ABP_URL}${path}`;
  const headers: Record<string, string> = {
    "Content-Type": "application/json",
  };
  if (ABP_AUTH_TOKEN) {
    headers["Authorization"] = `Bearer ${ABP_AUTH_TOKEN}`;
  }

  const response = await fetch(url, {
    method,
    headers,
    body: body ? JSON.stringify(body) : undefined,
  });

  const text = await response.text();
  if (!text) return {};

  try {
    return JSON.parse(text);
  } catch {
    throw new Error(`Invalid JSON response: ${text}`);
  }
}

// Tool definitions
const TOOLS: Tool[] = [
  // Tab Management
  {
    name: "browser_list_tabs",
    description: "List all open browser tabs",
    inputSchema: {
      type: "object",
      properties: {},
    },
  },
  {
    name: "browser_new_tab",
    description: "Create a new browser tab",
    inputSchema: {
      type: "object",
      properties: {
        url: { type: "string", description: "URL to navigate to" },
      },
    },
  },
  {
    name: "browser_close_tab",
    description: "Close a browser tab",
    inputSchema: {
      type: "object",
      properties: {
        tab_id: { type: "string", description: "ID of tab to close" },
      },
      required: ["tab_id"],
    },
  },
  {
    name: "browser_get_tab_info",
    description: "Get detailed information about a tab",
    inputSchema: {
      type: "object",
      properties: {
        tab_id: { type: "string", description: "ID of tab" },
      },
      required: ["tab_id"],
    },
  },

  // Navigation
  {
    name: "browser_navigate",
    description: "Navigate to a URL",
    inputSchema: {
      type: "object",
      properties: {
        tab_id: { type: "string", description: "Target tab ID" },
        url: { type: "string", description: "URL to navigate to" },
      },
      required: ["tab_id", "url"],
    },
  },
  {
    name: "browser_go_back",
    description: "Navigate back in history",
    inputSchema: {
      type: "object",
      properties: {
        tab_id: { type: "string", description: "Target tab ID" },
      },
      required: ["tab_id"],
    },
  },
  {
    name: "browser_go_forward",
    description: "Navigate forward in history",
    inputSchema: {
      type: "object",
      properties: {
        tab_id: { type: "string", description: "Target tab ID" },
      },
      required: ["tab_id"],
    },
  },
  {
    name: "browser_reload",
    description: "Reload the current page",
    inputSchema: {
      type: "object",
      properties: {
        tab_id: { type: "string", description: "Target tab ID" },
      },
      required: ["tab_id"],
    },
  },

  // Mouse Actions
  {
    name: "browser_click",
    description: "Click at coordinates on the page",
    inputSchema: {
      type: "object",
      properties: {
        tab_id: { type: "string", description: "Target tab ID" },
        x: { type: "number", description: "X coordinate" },
        y: { type: "number", description: "Y coordinate" },
      },
      required: ["tab_id", "x", "y"],
    },
  },

  // Keyboard Actions
  {
    name: "browser_type",
    description: "Type text at current focus position",
    inputSchema: {
      type: "object",
      properties: {
        tab_id: { type: "string", description: "Target tab ID" },
        text: { type: "string", description: "Text to type" },
      },
      required: ["tab_id", "text"],
    },
  },

  // Screenshots
  {
    name: "browser_screenshot",
    description: "Take a screenshot of the page with optional element markup",
    inputSchema: {
      type: "object",
      properties: {
        tab_id: { type: "string", description: "Target tab ID" },
        format: {
          type: "string",
          enum: ["png", "jpeg", "webp"],
          description: "Image format (default: webp)",
        },
        quality: {
          type: "number",
          description: "Image quality 1-100 for jpeg/webp",
        },
        markup: {
          type: "string",
          enum: ["none", "interactive", "clickable", "typeable", "inputs"],
          description: "Element markup overlay type",
        },
      },
      required: ["tab_id"],
    },
  },

  // JavaScript Execution
  {
    name: "browser_execute_javascript",
    description: "Execute JavaScript in the page context",
    inputSchema: {
      type: "object",
      properties: {
        tab_id: { type: "string", description: "Target tab ID" },
        expression: { type: "string", description: "JavaScript expression" },
      },
      required: ["tab_id", "expression"],
    },
  },
];

// Tool handlers
async function handleTool(
  name: string,
  args: Record<string, unknown>
): Promise<unknown> {
  switch (name) {
    // Tab Management
    case "browser_list_tabs":
      return abpRequest("GET", "/api/v1/tabs");

    case "browser_new_tab":
      return abpRequest("POST", "/api/v1/tabs", { url: args.url });

    case "browser_close_tab":
      return abpRequest("DELETE", `/api/v1/tabs/${args.tab_id}`);

    case "browser_get_tab_info":
      return abpRequest("GET", `/api/v1/tabs/${args.tab_id}`);

    // Navigation
    case "browser_navigate":
      return abpRequest("POST", `/api/v1/tabs/${args.tab_id}/navigate`, {
        url: args.url,
      });

    case "browser_go_back":
      return abpRequest("POST", `/api/v1/tabs/${args.tab_id}/back`);

    case "browser_go_forward":
      return abpRequest("POST", `/api/v1/tabs/${args.tab_id}/forward`);

    case "browser_reload":
      return abpRequest("POST", `/api/v1/tabs/${args.tab_id}/reload`);

    // Mouse Actions
    case "browser_click":
      return abpRequest("POST", `/api/v1/tabs/${args.tab_id}/click`, {
        x: args.x,
        y: args.y,
      });

    // Keyboard Actions
    case "browser_type":
      return abpRequest("POST", `/api/v1/tabs/${args.tab_id}/type`, {
        text: args.text,
      });

    // Screenshots
    case "browser_screenshot":
      return abpRequest("POST", `/api/v1/tabs/${args.tab_id}/screenshot`, {
        screenshot: {
          format: args.format || "webp",
          quality: args.quality || 80,
          markup: args.markup || "none",
        },
      });

    // JavaScript Execution
    case "browser_execute_javascript":
      return abpRequest("POST", `/api/v1/tabs/${args.tab_id}/execute`, {
        script: args.expression,
      });

    default:
      throw new Error(`Unknown tool: ${name}`);
  }
}

// Create and configure MCP server
const server = new Server(
  {
    name: "abp-browser",
    version: "1.0.0",
  },
  {
    capabilities: {
      tools: {},
      resources: {},
    },
  }
);

// List available tools
server.setRequestHandler(ListToolsRequestSchema, async () => {
  return { tools: TOOLS };
});

// Handle tool calls
server.setRequestHandler(CallToolRequestSchema, async (request) => {
  const { name, arguments: args } = request.params;

  try {
    const result = await handleTool(name, (args || {}) as Record<string, unknown>);
    return {
      content: [
        {
          type: "text",
          text: JSON.stringify(result, null, 2),
        },
      ],
    };
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error);
    return {
      content: [
        {
          type: "text",
          text: `Error: ${message}`,
        },
      ],
      isError: true,
    };
  }
});

// List available resources
server.setRequestHandler(ListResourcesRequestSchema, async () => {
  return {
    resources: [
      {
        uri: "browser://tabs",
        name: "Browser Tabs",
        description: "List of all open browser tabs",
        mimeType: "application/json",
      },
    ],
  };
});

// Read resource
server.setRequestHandler(ReadResourceRequestSchema, async (request) => {
  const { uri } = request.params;

  if (uri === "browser://tabs") {
    const tabs = await abpRequest("GET", "/api/v1/tabs");
    return {
      contents: [
        {
          uri,
          mimeType: "application/json",
          text: JSON.stringify(tabs, null, 2),
        },
      ],
    };
  }

  throw new Error(`Unknown resource: ${uri}`);
});

// Start the server
async function main() {
  const transport = new StdioServerTransport();
  await server.connect(transport);
  console.error("ABP MCP Server running on stdio");
  console.error(`Connecting to ABP at: ${ABP_URL}`);
}

main().catch((error) => {
  console.error("Fatal error:", error);
  process.exit(1);
});
