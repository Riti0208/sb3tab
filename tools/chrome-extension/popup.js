// Tab switching
document.querySelectorAll(".tab").forEach((tab) => {
  tab.addEventListener("click", () => {
    document.querySelectorAll(".tab").forEach((t) => t.classList.remove("active"));
    document.querySelectorAll(".panel").forEach((p) => p.classList.remove("active"));
    tab.classList.add("active");
    document.getElementById(tab.dataset.panel).classList.add("active");
  });
});

// Helper: generate QR code into container using qrcode-generator lib
function generateQR(container, data, size) {
  container.innerHTML = "";
  try {
    // typeNumber=0 means auto-detect
    const qr = qrcode(0, "L");
    qr.addData(data);
    qr.make();
    container.innerHTML = qr.createSvgTag({ cellSize: 4, margin: 4 });
    // Scale SVG to desired size
    const svg = container.querySelector("svg");
    if (svg) {
      svg.setAttribute("width", size);
      svg.setAttribute("height", size);
      svg.style.borderRadius = "8px";
      svg.style.border = "2px solid #eee";
    }
    return true;
  } catch (e) {
    container.textContent = `Error: ${e.message}`;
    container.style.color = "#c33";
    return false;
  }
}

// --- WiFi QR ---
chrome.storage.local.get(["wifiSsid"], (data) => {
  if (data.wifiSsid) document.getElementById("wifi-ssid").value = data.wifiSsid;
});

document.getElementById("gen-wifi").addEventListener("click", () => {
  const ssid = document.getElementById("wifi-ssid").value.trim();
  const pass = document.getElementById("wifi-pass").value;
  if (!ssid) return;

  chrome.storage.local.set({ wifiSsid: ssid });

  const qrData = `W:${ssid}\n${pass}`;
  const container = document.getElementById("qr-wifi");

  if (generateQR(container, qrData, 256)) {
    document.getElementById("wifi-info").textContent = `SSID: ${ssid}`;
    document.getElementById("wifi-info").style.color = "";
  }
});

// --- Project QR ---
chrome.tabs.query({ active: true, currentWindow: true }, (tabs) => {
  const url = tabs[0]?.url || "";
  const statusEl = document.getElementById("status");
  const infoEl = document.getElementById("project-info");

  const match = url.match(/scratch\.mit\.edu\/projects\/(\d+)/);
  if (!match) {
    statusEl.textContent = "Not a Scratch project page";
    statusEl.className = "error";
    return;
  }

  const projectId = match[1];
  const qrData = `S:${projectId}`;

  statusEl.textContent = `Project #${projectId}`;
  statusEl.className = "ok";

  generateQR(document.getElementById("qr-container"), qrData, 256);
  infoEl.textContent = qrData;
});
