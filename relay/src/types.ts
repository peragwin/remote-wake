import type { UpdateBindings } from "./updates";

export interface Env extends UpdateBindings {
  /** One Durable Object namespace; one instance per deviceId. */
  DEVICE: DurableObjectNamespace;
}
