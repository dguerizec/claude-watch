export interface Config {
  ssid: string;
  ip: string;
  timezone: string;
  fetch_interval: string;
  token_status: string;
  auth_url: string;
}

export interface DisplayConfig {
  screens: ScreenEntry[];
}

export interface ScreenEntry {
  id: number;
  name: string;
  enabled: boolean;
}

export const SCREEN_NAMES: Record<number, string> = {
  0: "Usage Values",
  1: "Graph 7-day",
  2: "Graph 7-day + history",
  3: "Graph 5-hour",
  4: "Clock",
};
