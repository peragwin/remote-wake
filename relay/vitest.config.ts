import { cloudflareTest } from "@cloudflare/vitest-pool-workers";
import { defineConfig } from "vitest/config";

// Tests run inside the real workerd runtime (Miniflare), so the Durable
// Object, WebSocket hibernation and alarms behave exactly as in production.
export default defineConfig({
  plugins: [cloudflareTest({ wrangler: { configPath: "./wrangler.toml" }, miniflare: { bindings: {
    UPDATE_CATALOG_URL: "https://raw.githubusercontent.com/example/firmware/main/channel.json",
    UPDATE_ALLOWED_REPOS: "example/firmware",
  } } })],
});
