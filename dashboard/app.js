/* Smart Water Network dashboard - API refresh every 5 seconds. */
const DEFAULT_API_URL = "http://10.87.94.6:8080/data";
const REFRESH_MS = 5000;
let apiUrl = DEFAULT_API_URL;
const $ = id => document.getElementById(id);
function num(...values) {
  for (const v of values) {
    if (v === null || v === undefined || v === "") continue;
    const n = Number(v);
    if (Number.isFinite(n)) return n;
  }
  return null;
}
function fmt(v, unit) {
  const n = num(v);
  return `${(n === null ? 0 : n).toFixed(2)} <small>${unit}</small>`;
}
function setText(id, value) { $(id).textContent = value == null ? "" : String(value); }
function setState(id, value, kind="neutral") {
  const e = $(id);
  e.textContent = value == null ? "" : String(value);
  e.classList.remove("ok","bad","neutral");
  e.classList.add(kind);
}
function isConnected(v) {
  const s = String(v || "").toUpperCase();
  return s === "CONNECTED" || (s.includes("CONNECT") && !s.includes("DISCONNECT"));
}
function render(d) {
  const a = d.node1 && typeof d.node1 === "object" ? d.node1 : {};
  const b = d.node2 && typeof d.node2 === "object" ? d.node2 : {};
  const f1 = num(d.node1Flow, d.node1_flow, a.flow, typeof d.node1 === "number" ? d.node1 : null);
  const f2 = num(d.node2Flow, d.node2_flow, b.flow, typeof d.node2 === "number" ? d.node2 : null);
  const p1 = num(d.node1Pressure, d.node1_pressure, a.pressure);
  const p2 = num(d.node2Pressure, d.node2_pressure, b.pressure);
  const s1 = String(d.node1Status ?? d.node1_status ?? a.status ?? "DISCONNECTED").toUpperCase();
  const s2 = String(d.node2Status ?? d.node2_status ?? b.status ?? "DISCONNECTED").toUpperCase();

  setText("node1Status", s1);
  setText("node2Status", s2);
  $("node1Status").className = "node-badge " + (isConnected(s1) ? "online" : "");
  $("node2Status").className = "node-badge " + (isConnected(s2) ? "online" : "");
  $("node1Flow").innerHTML = fmt(f1, "mL/s");
  $("node2Flow").innerHTML = fmt(f2, "mL/s");
  $("node1Pressure").innerHTML = fmt(p1, "bar");
  $("node2Pressure").innerHTML = fmt(p2, "bar");

  const fd = num(d.flowDifference, d.flow_difference, d.difference, (f1 ?? 0) - (f2 ?? 0));
  const pd = num(d.pressureDifference, d.pressure_difference, d.pressureDiff,
                 (p1 !== null && p2 !== null) ? p1-p2 : null);
  $("flowDifference").innerHTML = fmt(fd, "mL/s");
  $("pressureDifference").innerHTML = fmt(pd, "bar");

  let leak = d.leakStatus ?? d.leak_status ?? d.leak;
  if (typeof leak === "boolean") leak = leak ? "LEAK DETECTED" : "NORMAL";
  if (leak == null || leak === "") {
    leak = (isConnected(s1) && isConnected(s2) && f1 !== null && f2 !== null)
      ? (((f1 > 0 && f2 === 0) || (f1-f2 > 0.5)) ? "LEAK DETECTED" : "NORMAL")
      : "WAITING FOR DATA";
  }
  const leakBad = String(leak).toLowerCase().includes("leak") && !String(leak).toLowerCase().includes("no leak");
  setState("leakStatus", leak, leakBad ? "bad" : (String(leak).toLowerCase().includes("normal") ? "ok" : "neutral"));
  setText("leakDetails", d.leakDetails ?? d.leak_details ?? (leakBad ? "Abnormal flow pattern detected." : "Based on the latest node readings."));

  // Show valve state for all five conditions:
  // 1 both disconnected -> OFF / CLOSED
  // 2 both connected, both flow readings zero -> OFF / CLOSED
  // 3 Node 1 zero, Node 2 positive -> OFF / CLOSED
  // 4 Node 1 positive, Node 2 zero -> OFF / CLOSED
  // 5 both positive -> close if flow difference > 0.5 mL/s; otherwise open.
  const flowDiffForValve = num(d.flowDifference, d.flow_difference, d.difference,
                               (f1 !== null && f2 !== null) ? f1 - f2 : null);
  const bothDisconnected = !isConnected(s1) && !isConnected(s2);
  const bothZero = isConnected(s1) && isConnected(s2) && f1 === 0 && f2 === 0;
  const node1ZeroNode2Positive = isConnected(s1) && isConnected(s2) && f1 === 0 && f2 > 0;
  const node1PositiveNode2Zero = isConnected(s1) && isConnected(s2) && f1 > 0 && f2 === 0;
  const bothPositive = isConnected(s1) && isConnected(s2) && f1 > 0 && f2 > 0;

  let valveMessage = "VALVE IS CLOSED";
  let valveKind = "bad";
  let valveDetail = "Valve command OFF; normally-closed valve is expected to be closed.";

  if (bothDisconnected || bothZero || node1ZeroNode2Positive || node1PositiveNode2Zero) {
    valveMessage = "VALVE IS CLOSED";
    valveKind = "bad";
    valveDetail = "Valve command OFF for this condition; verify physical valve state.";
  } else if (bothPositive) {
    if (flowDiffForValve !== null && flowDiffForValve > 0.5) {
      valveMessage = "VALVE IS CLOSED";
      valveKind = "bad";
      valveDetail = "Flow difference is greater than 0.5 mL/s; isolation command OFF.";
    } else {
      valveMessage = "VALVE IS OPEN";
      valveKind = "ok";
      valveDetail = "Flow difference is at or below 0.5 mL/s; valve command ON.";
    }
  } else {
    // When API includes an explicit condition label, use it as a fallback
    // if the node status/readings are not populated in the expected shape.
    const condition = String(d.condition ?? d.conditionId ?? d.mode ?? "").toLowerCase();
    if (["both_positive","condition5"].includes(condition)) {
      if (flowDiffForValve !== null && flowDiffForValve > 0.5) {
        valveMessage = "VALVE IS CLOSED";
        valveKind = "bad";
        valveDetail = "Flow difference is greater than 0.5 mL/s; isolation command OFF.";
      } else {
        valveMessage = "VALVE IS OPEN";
        valveKind = "ok";
        valveDetail = "Flow difference is at or below 0.5 mL/s; valve command ON.";
      }
    } else if (["both_disconnected","condition1","both_zero","condition2",
                "node1_zero_node2_positive","condition3",
                "node1_positive_node2_zero","condition4"].includes(condition)) {
      valveMessage = "VALVE IS CLOSED";
      valveKind = "bad";
      valveDetail = "Valve command OFF for this condition; verify physical valve state.";
    } else {
      // Preserve the actual API valve value only if condition cannot be classified.
      const supplied = d.valveStatus ?? d.valveControl ?? d.valve_status;
      if (supplied !== undefined && supplied !== null && supplied !== "") {
        valveMessage = typeof supplied === "boolean"
          ? (supplied ? "VALVE IS OPEN" : "VALVE IS CLOSED")
          : String(supplied);
        valveKind = valveMessage.toLowerCase().includes("close") || valveMessage.toLowerCase().includes("off") ? "bad" : "ok";
        valveDetail = d.isolationStatus ?? d.isolation_status ?? "";
      }
    }
  }
  setState("valveStatus", valveMessage, valveKind);
  setText("valveDetails", valveDetail);

  const recommendation = d.isolationRecommendation ?? d.isolation_recommendation ?? d.recommendation ?? "";
  setText("recommendation", recommendation || "Waiting for isolation recommendation from QNX.");
  setText("isolationTitle", d.isolationTitle ?? d.isolation_title ?? (leakBad ? "Isolation recommended" : "Isolation recommendation"));
  $("isolationIcon").textContent = leakBad ? "!" : "i";

  const udp = d.udpStatus ?? d.udp_status ?? d.udp;
  const online = udp != null
    ? !String(udp).toLowerCase().includes("not received") && !String(udp).toLowerCase().includes("disconnected")
    : (isConnected(s1) || isConnected(s2));
  setText("connectionStatus", online ? "QNX API connected / data received" : "QNX API connected / nodes disconnected");
  $("connectionDot").classList.toggle("online", online);
  $("connectionDot").classList.toggle("offline", !online);
  setText("lastUpdate", `Last update: ${new Date().toLocaleTimeString()}`);
}
async function refresh() {
  try {
    const response = await fetch(apiUrl, {cache:"no-store"});
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    render(await response.json());
  } catch (err) {
    setText("connectionStatus", "Not connected to QNX API");
    $("connectionDot").classList.remove("online");
    $("connectionDot").classList.add("offline");
    setText("lastUpdate", `Connection error: ${err.message}`);
    setText("recommendation", "Start chinnaga on QNX and verify the Pi IP address, port 8080, and /data endpoint.");
    setState("leakStatus", "WAITING FOR DATA", "neutral");
    setState("valveStatus", "", "neutral");
    setText("valveDetails", "");
  }
}
document.addEventListener("DOMContentLoaded", () => {
  refresh();
  window.setInterval(refresh, REFRESH_MS);
});
