import { HeadroomPlugin } from "headroom-opencode";
// Kill switch for presets whose traffic must bypass the proxy (e.g.
// opencode-zen-free: zen requests carry placeholder apiKey "public", which the
// proxy forwards upstream and causes random "Incorrect API key" agent deaths).
// With HEADROOM_ENABLED=0 this plugin registers nothing: no transport shim,
// no shell.env, no headroom_retrieve tool.
export default async function plugin(input) {
  if (process.env.HEADROOM_ENABLED === "0") return {};
  return HeadroomPlugin(input, { proxyUrl: process.env.HEADROOM_PROXY_URL ?? "http://127.0.0.1:8787" });
}
