import { Config, DisplayConfig, ScreenEntry, SCREEN_NAMES } from "./types";

// ── Tab navigation ──────────────────────────────────────────────────

function initTabs() {
  document.querySelectorAll<HTMLElement>(".tab").forEach((tab) => {
    tab.addEventListener("click", () => {
      document.querySelectorAll(".tab").forEach((t) => t.classList.remove("active"));
      document.querySelectorAll(".panel").forEach((p) => p.classList.remove("active"));
      tab.classList.add("active");
      document.getElementById(tab.dataset.panel!)!.classList.add("active");
    });
  });
}

// ── Toast notification ──────────────────────────────────────────────

function toast(msg: string, error = false) {
  const el = document.getElementById("toast")!;
  el.textContent = msg;
  el.className = "toast " + (error ? "error" : "ok");
  el.style.display = "block";
  setTimeout(() => (el.style.display = "none"), 2500);
}

// ── Settings tab ────────────────────────────────────────────────────

async function loadConfig() {
  try {
    const resp = await fetch("/api/config");
    const cfg: Config = await resp.json();
    (document.getElementById("tz") as HTMLSelectElement).value =
      cfg.timezone || "CET-1CEST,M3.5.0,M10.5.0/3";
    (document.getElementById("fi") as HTMLSelectElement).value =
      cfg.fetch_interval || "10";
    document.getElementById("wifi-info")!.textContent =
      `WiFi: ${cfg.ssid} | IP: ${cfg.ip}`;
    document.getElementById("token-status")!.textContent =
      `Token: ${cfg.token_status}`;

    // Set auth link
    const authLink = document.getElementById("auth-link") as HTMLAnchorElement;
    authLink.href = cfg.auth_url;
  } catch {
    toast("Failed to load config", true);
  }
}

function initSettings() {
  document.getElementById("settings-form")!.addEventListener("submit", async (e) => {
    e.preventDefault();
    const body = new URLSearchParams({
      ssid: (document.getElementById("ss") as HTMLInputElement).value,
      password: (document.getElementById("pw") as HTMLInputElement).value,
      timezone: (document.getElementById("tz") as HTMLSelectElement).value,
      fetch_int: (document.getElementById("fi") as HTMLSelectElement).value,
    });
    try {
      await fetch("/api/config", { method: "POST", body });
      toast("Settings saved!");
    } catch {
      toast("Save failed", true);
    }
  });

  document.getElementById("btn-wifi-reset")!.addEventListener("click", async () => {
    if (!confirm("Reset WiFi and reboot?")) return;
    await fetch("/api/wifi-reset", { method: "POST" });
    document.body.innerHTML = "<h2>Rebooting...</h2>";
  });
}

// ── Account tab ─────────────────────────────────────────────────────

function initAccount() {
  document.getElementById("btn-authorize")!.addEventListener("click", async () => {
    const code = (document.getElementById("auth-code") as HTMLInputElement).value.trim();
    if (!code) {
      toast("Paste the code first", true);
      return;
    }
    try {
      const resp = await fetch("/api/oauth/exchange", {
        method: "POST",
        headers: { "Content-Type": "application/x-www-form-urlencoded" },
        body: `code=${encodeURIComponent(code)}`,
      });
      const data = await resp.json();
      if (data.ok) {
        toast("Authorized!");
        loadConfig(); // refresh token status
      } else {
        toast("Authorization failed", true);
      }
    } catch {
      toast("Authorization failed", true);
    }
  });

  document.getElementById("copy-auth-url")!.addEventListener("click", (e) => {
    e.preventDefault();
    const url = (document.getElementById("auth-link") as HTMLAnchorElement).href;
    navigator.clipboard.writeText(url).then(() => {
      (e.target as HTMLElement).textContent = "Copied!";
      setTimeout(() => ((e.target as HTMLElement).textContent = "Copy URL"), 2000);
    });
  });
}

// ── Display tab ─────────────────────────────────────────────────────

let displayScreens: ScreenEntry[] = [];

async function loadDisplay() {
  try {
    const resp = await fetch("/api/display");
    const cfg: DisplayConfig = await resp.json();
    displayScreens = cfg.screens;
    renderScreenList();
  } catch {
    toast("Failed to load display config", true);
  }
}

function renderScreenList() {
  const list = document.getElementById("screen-list")!;
  list.innerHTML = "";
  displayScreens.forEach((screen, idx) => {
    const row = document.createElement("div");
    row.className = "screen-row";
    row.innerHTML = `
      <label class="screen-toggle">
        <input type="checkbox" ${screen.enabled ? "checked" : ""} data-idx="${idx}">
        <span>${screen.name}</span>
      </label>
      <div class="screen-arrows">
        <button class="arr" data-dir="up" data-idx="${idx}" ${idx === 0 ? "disabled" : ""}>&#9650;</button>
        <button class="arr" data-dir="down" data-idx="${idx}" ${idx === displayScreens.length - 1 ? "disabled" : ""}>&#9660;</button>
      </div>
    `;
    list.appendChild(row);
  });

  // Checkbox handlers
  list.querySelectorAll<HTMLInputElement>("input[type=checkbox]").forEach((cb) => {
    cb.addEventListener("change", () => {
      displayScreens[Number(cb.dataset.idx)].enabled = cb.checked;
    });
  });

  // Arrow handlers
  list.querySelectorAll<HTMLButtonElement>(".arr").forEach((btn) => {
    btn.addEventListener("click", () => {
      const idx = Number(btn.dataset.idx);
      const dir = btn.dataset.dir === "up" ? -1 : 1;
      const other = idx + dir;
      [displayScreens[idx], displayScreens[other]] = [displayScreens[other], displayScreens[idx]];
      renderScreenList();
    });
  });
}

function initDisplay() {
  document.getElementById("btn-save-display")!.addEventListener("click", async () => {
    const payload = displayScreens.map((s) => `${s.id}:${s.enabled ? 1 : 0}`).join(",");
    try {
      await fetch("/api/display", {
        method: "POST",
        headers: { "Content-Type": "application/x-www-form-urlencoded" },
        body: `screens=${encodeURIComponent(payload)}`,
      });
      toast("Display config saved!");
    } catch {
      toast("Save failed", true);
    }
  });

  loadDisplay();
}

// ── Init ────────────────────────────────────────────────────────────

document.addEventListener("DOMContentLoaded", () => {
  initTabs();
  initSettings();
  initAccount();
  initDisplay();
  loadConfig();
});
