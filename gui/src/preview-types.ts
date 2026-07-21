export type PreviewState = "idle" | "starting" | "discovering" | "configuring" | "waiting_data" | "streaming" | "stopping" | "stopped" | "error";

export type PreviewNotice = {
  tone: "info" | "warning" | "error";
  title: string;
  detail: string;
  hint?: string;
} | null;
