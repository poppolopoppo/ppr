import { HeadroomPlugin } from "headroom-opencode";
// Per-model Headroom routing: proxy OpenAI-hosted traffic, bypass the proxy
// for opencode zen hosts (zen requests carry placeholder apiKey "public",
// which the proxy forwards upstream and causes random "Incorrect API key"
// agent deaths).
//
// Why host-based, not model-id: the upstream transport shim routes at the
// fetch layer (shouldRoute in headroom-opencode/dist proxies every
// non-loopback http(s) URL, with no host allowlist and no chat.* hook
// upstream), so the deterministic cut is the request hostname. This also
// avoids racy global toggling when oracle (openai/*) and orchestrator
// (opencode/* zen-free) call concurrently, and needs no per-model
// bookkeeping as presets evolve.
//
// Default bypass: "<anything>.opencode.ai" (covers api.opencode.ai and
// opencode.ai). Extend via HEADROOM_BYPASS_HOSTS="host[,host...]" (exact or
// parent-domain match). HEADROOM_ENABLED=0 remains the master kill-switch:
// the plugin registers nothing (no transport shim, no shell.env,
// no headroom_retrieve tool).
const DEFAULT_BYPASS_HOSTS = ["opencode.ai"];

function bypassHosts() {
  const extra = (process.env.HEADROOM_BYPASS_HOSTS ?? "")
    .split(",")
    .map((h) => h.trim().toLowerCase().replace(/^\./, ""))
    .filter(Boolean);
  return [...DEFAULT_BYPASS_HOSTS, ...extra];
}

function isBypassedHost(hostname, hosts) {
  const name = hostname.toLowerCase();
  return hosts.some((h) => name === h || name.endsWith(`.${h}`));
}

function upstreamUrlOf(input) {
  try {
    if (typeof input === "string" || input instanceof URL) return new URL(input);
    if (typeof Request !== "undefined" && input instanceof Request) return new URL(input.url);
  } catch {
    // Unparseable/relative input: let the wrapped transport decide.
  }
  return undefined;
}

export default async function plugin(input) {
  if (process.env.HEADROOM_ENABLED === "0") return {};
  const proxyUrl = process.env.HEADROOM_PROXY_URL ?? "http://127.0.0.1:8787";
  const directFetch = globalThis.fetch;
  const hosts = bypassHosts();
  const pluginResult = await HeadroomPlugin(input, { proxyUrl });
  const headroomFetch = globalThis.fetch;
  async function routedFetch(...args) {
    const upstream = upstreamUrlOf(args[0]);
    if (upstream && isBypassedHost(upstream.hostname, hosts)) {
      return directFetch(...args);
    }
    return headroomFetch(...args);
  }
  globalThis.fetch = routedFetch;
  const headroomDispose = pluginResult.dispose;
  return {
    ...pluginResult,
    dispose: async () => {
      if (globalThis.fetch === routedFetch) globalThis.fetch = directFetch;
      await headroomDispose?.();
    },
  };
}
