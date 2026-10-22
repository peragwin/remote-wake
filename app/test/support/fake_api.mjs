// The real api.js (sendCommand goes through the harness's fetch fake), minus
// the presence poller, whose 15 s timers have nothing to do with these tests.
export * from '../../src/api.js';
export function createPresencePoller() {
  return { start() {}, stop() {}, refresh: async () => {} };
}
