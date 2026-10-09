SMART WATER NETWORK DASHBOARD — VALVE STATUS FOR ALL 5 CONDITIONS
====================================================================

Valve Status behavior:
1. Both nodes disconnected: VALVE IS CLOSED (command OFF).
2. Both nodes connected and both flow readings are zero: VALVE IS CLOSED (command OFF).
3. Node 1 flow zero and Node 2 flow positive: VALVE IS CLOSED (command OFF).
4. Node 1 flow positive and Node 2 flow zero: VALVE IS CLOSED (command OFF).
5. Both flow readings positive:
   - Flow difference > 0.5 mL/s: VALVE IS CLOSED (command OFF).
   - Flow difference <= 0.5 mL/s: VALVE IS OPEN (command ON).

This is the requested dashboard display logic. Actual physical valve movement must
be verified with relay wiring and QNX GPIO/valve control code.

API URL is configured in dashboard/app.js:
const DEFAULT_API_URL = "http://10.87.94.6:8080/data";
Replace with your actual Raspberry Pi/QNX IP.

Dashboard files: index.html, style.css, app.js.
QNX API must return node status/readings and flow difference accurately.
