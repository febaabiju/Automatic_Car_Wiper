/**
 * Automatic Car Wiper - IoT Dashboard Application
 * Connects to FastAPI backend (dynamically resolves origin on Render or localhost).
 * Polls for live telemetry every 3 seconds without generating fake/dummy data.
 */

// Dynamically resolve API Base URL (Render Cloud URL, Local FastAPI, or Static Dev server)
function resolveApiBaseUrl() {
  if (window.location.protocol.startsWith('http')) {
    // If run on a separate local static dev port (e.g., Live Server :3000 or :5500)
    if (window.location.port === '3000' || window.location.port === '5500') {
      return 'http://127.0.0.1:8000';
    }
    // When served directly by FastAPI or hosted on Render
    return window.location.origin;
  }
  // Fallback for file:/// direct opening
  return 'http://127.0.0.1:8000';
}

const API_BASE_URL = resolveApiBaseUrl();
const POLL_INTERVAL_MS = 3000;
const OFFLINE_THRESHOLD_SECONDS = 30;

// Application State
let rainChart = null;
let pollTimer = null;
let isFetching = false;
let latestReadingCache = null;

// DOM Elements Cache
const elements = {
  backendPill: document.getElementById('backend-status-pill'),
  backendText: document.getElementById('backend-status-text'),
  esp32Pill: document.getElementById('esp32-status-pill'),
  esp32Text: document.getElementById('esp32-status-text'),
  esp32Badge: document.getElementById('esp32-badge'),
  lastSeenText: document.getElementById('last-seen-text'),

  systemAlert: document.getElementById('system-alert'),
  systemAlertMessage: document.getElementById('system-alert-message'),

  rainValueDisplay: document.getElementById('rain-value-display'),
  rainMeterBar: document.getElementById('rain-meter-bar'),
  rainIntensityText: document.getElementById('rain-intensity-text'),

  rainStatusBadge: document.getElementById('rain-status-badge'),
  rainStatusSubtext: document.getElementById('rain-status-subtext'),

  wiperStatusBadge: document.getElementById('wiper-status-badge'),
  wiperStatusSubtext: document.getElementById('wiper-status-subtext'),

  wiperSpeedDisplay: document.getElementById('wiper-speed-display'),
  wiperSpeedSubtext: document.getElementById('wiper-speed-subtext'),

  tableRecordCount: document.getElementById('table-record-count'),
  tableBody: document.getElementById('readings-table-body'),
  emptyStateRow: document.getElementById('empty-state-row'),

  refreshBtn: document.getElementById('manual-refresh-btn'),
  apiUrlLabel: document.getElementById('api-url-label'),
};

/**
 * Initialize Chart.js Real-Time Line Graph
 */
function initChart() {
  const canvas = document.getElementById('rainChart');
  if (!canvas) return;

  const ctx = canvas.getContext('2d');
  const gradient = ctx.createLinearGradient(0, 0, 0, 300);
  gradient.addColorStop(0, 'rgba(59, 130, 246, 0.35)');
  gradient.addColorStop(1, 'rgba(59, 130, 246, 0.0)');

  rainChart = new Chart(ctx, {
    type: 'line',
    data: {
      labels: [],
      datasets: [
        {
          label: 'Rain Sensor Value (ADC)',
          data: [],
          borderColor: '#3b82f6',
          backgroundColor: gradient,
          fill: true,
          tension: 0.3,
          borderWidth: 2,
          pointBackgroundColor: '#60a5fa',
          pointBorderColor: '#16223e',
          pointBorderWidth: 2,
          pointRadius: 3,
          pointHoverRadius: 6,
        },
      ],
    },
    options: {
      responsive: true,
      maintainAspectRatio: false,
      animation: { duration: 350 },
      scales: {
        x: {
          grid: { color: 'rgba(255, 255, 255, 0.05)' },
          ticks: {
            color: '#94a3b8',
            font: { family: 'Inter', size: 11 },
            maxRotation: 0,
            autoSkip: true,
            maxTicksLimit: 10,
          },
        },
        y: {
          min: 0,
          max: 4095,
          grid: { color: 'rgba(255, 255, 255, 0.05)' },
          ticks: {
            color: '#94a3b8',
            font: { family: 'Inter', size: 11 },
            stepSize: 1024,
            callback: function (val) {
              if (val === 0) return '0 (Wet)';
              if (val === 4095) return '4095 (Dry)';
              return val;
            },
          },
        },
      },
      plugins: {
        legend: { display: false },
        tooltip: {
          backgroundColor: '#111a30',
          titleColor: '#f8fafc',
          bodyColor: '#93c5fd',
          borderColor: '#203055',
          borderWidth: 1,
          padding: 10,
          displayColors: false,
          callbacks: {
            label: function (context) {
              return `Rain Value: ${context.parsed.y} ADC`;
            },
          },
        },
      },
    },
  });
}

/**
 * Format timestamps for display
 */
function formatTime(isoString) {
  if (!isoString) return '--:--:--';
  const d = new Date(isoString);
  return d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' });
}

function formatDateTime(isoString) {
  if (!isoString) return '--';
  const d = new Date(isoString);
  return `${d.toLocaleDateString()} ${d.toLocaleTimeString()}`;
}

function escapeHtml(str) {
  if (typeof str !== 'string') return String(str ?? '');
  return str.replace(/[&<>'"]/g, function (tag) {
    const chars = {
      '&': '&amp;',
      '<': '&lt;',
      '>': '&gt;',
      "'": '&#39;',
      '"': '&quot;',
    };
    return chars[tag] || tag;
  });
}

/**
 * Check Backend Health
 */
async function checkBackendHealth() {
  try {
    const response = await fetch(`${API_BASE_URL}/api/health`, {
      method: 'GET',
      headers: { Accept: 'application/json' },
    });

    if (response.ok) {
      const data = await response.json();
      elements.backendPill.className = 'status-pill status-online';
      elements.backendText.textContent = data.supabase_configured
        ? 'Backend: Connected'
        : 'Backend: DB Unconfigured';
      hideAlert();
      return true;
    } else {
      throw new Error(`Health check returned HTTP ${response.status}`);
    }
  } catch (err) {
    elements.backendPill.className = 'status-pill status-offline';
    elements.backendText.textContent = 'Backend: Offline';
    showAlert(
      `Cannot connect to FastAPI backend at ${API_BASE_URL}. Ensure uvicorn or Docker container is running.`
    );
    return false;
  }
}

/**
 * Fetch and Update Latest Reading
 */
async function fetchLatestReading() {
  try {
    const response = await fetch(`${API_BASE_URL}/api/latest`, {
      method: 'GET',
      headers: { Accept: 'application/json' },
    });

    if (response.status === 404) {
      // Backend is operational, but no telemetry in database yet (clean empty state)
      handleEmptyState();
      return null;
    }

    if (!response.ok) {
      const errorJson = await response.json().catch(() => ({}));
      throw new Error(errorJson.detail || `HTTP ${response.status}`);
    }

    const payload = await response.json();
    const reading = payload.data;
    latestReadingCache = reading;
    updateKPIs(reading);
    return reading;
  } catch (err) {
    handleFetchError(err.message);
    return null;
  }
}

/**
 * Fetch and Update Readings History (Last 30 records)
 */
async function fetchReadingsHistory() {
  try {
    const response = await fetch(`${API_BASE_URL}/api/readings`, {
      method: 'GET',
      headers: { Accept: 'application/json' },
    });

    if (!response.ok) {
      const errorJson = await response.json().catch(() => ({}));
      throw new Error(errorJson.detail || `HTTP ${response.status}`);
    }

    const payload = await response.json();
    const readings = payload.data || [];
    updateTable(readings);
    updateChart(readings);
  } catch (err) {
    console.warn('Could not fetch historical readings:', err.message);
  }
}

/**
 * Update KPI Cards with Latest Values
 */
function updateKPIs(reading) {
  if (!reading) {
    handleEmptyState();
    return;
  }

  // 1. Rain Sensor Value
  const rainVal = Number(reading.rain_value);
  elements.rainValueDisplay.textContent = Number.isNaN(rainVal) ? '--' : rainVal;

  // Dryness percentage (0 = wettest, 4095 = completely dry)
  const percent = Math.min(100, Math.max(0, Math.round((rainVal / 4095) * 100)));
  elements.rainMeterBar.style.width = `${percent}%`;

  if (rainVal < 1700) {
    elements.rainIntensityText.textContent = 'High Moisture (Heavy Rain)';
  } else if (rainVal < 3000) {
    elements.rainIntensityText.textContent = 'Moderate Moisture (Light Rain)';
  } else {
    elements.rainIntensityText.textContent = 'Low Moisture (Dry Windshield)';
  }

  // 2. Rain Status Badge
  const statusStr = (reading.rain_status || 'Dry').trim();
  elements.rainStatusBadge.textContent = statusStr;

  const statusLower = statusStr.toLowerCase();
  elements.rainStatusBadge.className = 'badge';
  if (statusLower.includes('heavy')) {
    elements.rainStatusBadge.classList.add('badge-heavy-rain');
  } else if (statusLower.includes('light') || statusLower.includes('moderate')) {
    elements.rainStatusBadge.classList.add('badge-light-rain');
  } else if (statusLower.includes('dry') || statusLower.includes('no rain')) {
    elements.rainStatusBadge.classList.add('badge-dry');
  } else {
    elements.rainStatusBadge.classList.add('badge-neutral');
  }
  elements.rainStatusSubtext.textContent = `Condition: ${statusStr}`;

  // 3. Wiper Status
  const isWiperOn = Boolean(reading.wiper_on);
  elements.wiperStatusBadge.textContent = isWiperOn ? 'ON' : 'OFF';
  elements.wiperStatusBadge.className = `badge ${isWiperOn ? 'badge-on' : 'badge-off'}`;
  elements.wiperStatusSubtext.textContent = isWiperOn ? 'Servo: Active Sweeping' : 'Servo: Parked';

  // 4. Wiper Speed (Consistent with Off, Slow, Fast)
  const speedStr = (reading.wiper_speed || (isWiperOn ? 'Fast' : 'Off')).trim();
  elements.wiperSpeedDisplay.textContent = speedStr;
  elements.wiperSpeedSubtext.textContent = isWiperOn ? `Sweep Rate: ${speedStr}` : 'Motor Stopped';

  // 5. ESP32 Device Status (Check 30-second timeout)
  updateESP32OnlineStatus(reading);
}

/**
 * Determine ESP32 Online / Offline Status
 */
function updateESP32OnlineStatus(reading) {
  if (!reading || !reading.created_at) {
    setESP32Offline('No telemetry received yet');
    return;
  }

  const readingTime = new Date(reading.created_at).getTime();
  const now = new Date().getTime();
  const diffSeconds = Math.max(0, Math.floor((now - readingTime) / 1000));

  const isOnline = diffSeconds <= OFFLINE_THRESHOLD_SECONDS;

  if (isOnline) {
    elements.esp32Pill.className = 'status-pill status-online';
    elements.esp32Text.textContent = `ESP32: Online`;
    elements.esp32Badge.className = 'badge badge-online';
    elements.esp32Badge.textContent = 'Online';
    elements.lastSeenText.textContent = `Live: updated ${diffSeconds}s ago (${escapeHtml(reading.device_id || 'ESP32')})`;
  } else {
    elements.esp32Pill.className = 'status-pill status-offline';
    elements.esp32Text.textContent = 'ESP32: Offline';
    elements.esp32Badge.className = 'badge badge-offline';
    elements.esp32Badge.textContent = 'Offline';

    let elapsed = `${diffSeconds}s ago`;
    if (diffSeconds >= 60) {
      const minutes = Math.floor(diffSeconds / 60);
      elapsed = `${minutes}m ago`;
    }
    elements.lastSeenText.textContent = `Last seen: ${elapsed} (${escapeHtml(reading.device_id || 'ESP32')})`;
  }
}

function setESP32Offline(reason) {
  elements.esp32Pill.className = 'status-pill status-offline';
  elements.esp32Text.textContent = 'ESP32: Offline';
  elements.esp32Badge.className = 'badge badge-offline';
  elements.esp32Badge.textContent = 'Offline';
  elements.lastSeenText.textContent = reason || 'No connection';
}

/**
 * Update Chart.js Graph with Chronological Telemetry
 */
function updateChart(readings) {
  if (!rainChart) return;

  if (!readings || readings.length === 0) {
    rainChart.data.labels = [];
    rainChart.data.datasets[0].data = [];
    rainChart.update();
    return;
  }

  // Backend orders by created_at DESC (newest first).
  // Graph must show time flowing from left to right (oldest to newest).
  const chronological = [...readings].reverse();

  const labels = chronological.map((r) => formatTime(r.created_at));
  const values = chronological.map((r) => r.rain_value);

  rainChart.data.labels = labels;
  rainChart.data.datasets[0].data = values;
  rainChart.update();
}

/**
 * Update Historical Readings Table (Newest first)
 */
function updateTable(readings) {
  if (!elements.tableBody) return;

  if (!readings || readings.length === 0) {
    elements.tableRecordCount.textContent = '0 Records';
    elements.tableBody.innerHTML = `
      <tr id="empty-state-row">
        <td colspan="6" class="empty-state-cell">
          <div class="empty-state-content">
            <svg width="40" height="40" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round">
              <circle cx="12" cy="12" r="10"></circle>
              <line x1="12" y1="8" x2="12" y2="12"></line>
              <line x1="12" y1="16" x2="12.01" y2="16"></line>
            </svg>
            <h4>No sensor readings recorded yet</h4>
            <p>Connect your ESP32 or send a POST request to <code>/api/readings</code> to see live data.</p>
          </div>
        </td>
      </tr>
    `;
    return;
  }

  elements.tableRecordCount.textContent = `${readings.length} Record${readings.length === 1 ? '' : 's'}`;

  const rowsHtml = readings.map((r) => {
    const isWiperOn = Boolean(r.wiper_on);
    const rainStatus = (r.rain_status || 'Dry').trim();
    const wiperSpeed = (r.wiper_speed || (isWiperOn ? 'Fast' : 'Off')).trim();

    const statusLower = rainStatus.toLowerCase();
    let rainBadgeClass = 'badge-neutral';
    if (statusLower.includes('heavy')) rainBadgeClass = 'badge-heavy-rain';
    else if (statusLower.includes('light') || statusLower.includes('moderate')) rainBadgeClass = 'badge-light-rain';
    else if (statusLower.includes('dry') || statusLower.includes('no rain')) rainBadgeClass = 'badge-dry';

    const wiperBadgeClass = isWiperOn ? 'badge-on' : 'badge-off';

    const speedLower = wiperSpeed.toLowerCase();
    let speedBadgeClass = 'badge-neutral';
    if (speedLower === 'fast') speedBadgeClass = 'badge-heavy-rain';
    else if (speedLower === 'slow') speedBadgeClass = 'badge-light-rain';
    else if (speedLower === 'off') speedBadgeClass = 'badge-off';

    return `
      <tr>
        <td class="table-time">${formatDateTime(r.created_at)}</td>
        <td class="table-code">${escapeHtml(r.device_id || 'ESP32')}</td>
        <td><strong>${r.rain_value}</strong> <span style="color:var(--text-muted);font-size:0.75rem;">ADC</span></td>
        <td><span class="badge ${rainBadgeClass}" style="font-size:0.8rem;padding:4px 10px;">${escapeHtml(rainStatus)}</span></td>
        <td><span class="badge ${wiperBadgeClass}" style="font-size:0.8rem;padding:4px 10px;">${isWiperOn ? 'ON' : 'OFF'}</span></td>
        <td><span class="badge ${speedBadgeClass}" style="font-size:0.8rem;padding:4px 10px;">${escapeHtml(wiperSpeed)}</span></td>
      </tr>
    `;
  }).join('');

  elements.tableBody.innerHTML = rowsHtml;
}

/**
 * Handle Empty State (Database has no rows)
 */
function handleEmptyState() {
  elements.rainValueDisplay.textContent = '--';
  elements.rainMeterBar.style.width = '0%';
  elements.rainIntensityText.textContent = 'Waiting for sensor telemetry';

  elements.rainStatusBadge.textContent = 'No Data';
  elements.rainStatusBadge.className = 'badge badge-neutral';
  elements.rainStatusSubtext.textContent = 'Condition: Waiting for data';

  elements.wiperStatusBadge.textContent = '--';
  elements.wiperStatusBadge.className = 'badge badge-neutral';
  elements.wiperStatusSubtext.textContent = 'Servo: Idle';

  elements.wiperSpeedDisplay.textContent = '--';
  elements.wiperSpeedSubtext.textContent = 'Sweep Rate: --';

  setESP32Offline('Waiting for first reading');
  updateTable([]);
  updateChart([]);
}

/**
 * Handle API Error
 */
function handleFetchError(message) {
  showAlert(`Backend error: ${message}`);
  setESP32Offline('Backend communication issue');
}

/**
 * Banner Alerts
 */
function showAlert(message) {
  elements.systemAlertMessage.textContent = message;
  elements.systemAlert.classList.remove('hidden');
}

function hideAlert() {
  elements.systemAlert.classList.add('hidden');
}

/**
 * Master Polling Cycle
 */
async function pollDashboard() {
  if (isFetching) return;
  isFetching = true;

  try {
    const isHealthy = await checkBackendHealth();
    if (isHealthy) {
      await Promise.all([fetchLatestReading(), fetchReadingsHistory()]);
    }
  } catch (err) {
    console.error('Polling exception:', err);
  } finally {
    isFetching = false;
  }
}

/**
 * Manual Refresh Action
 */
async function handleManualRefresh() {
  elements.refreshBtn.classList.add('spinning');
  await pollDashboard();
  setTimeout(() => {
    elements.refreshBtn.classList.remove('spinning');
  }, 400);
}

/**
 * Application Lifecycle Startup
 */
document.addEventListener('DOMContentLoaded', () => {
  // Update connected API URL in footer
  if (elements.apiUrlLabel) {
    elements.apiUrlLabel.textContent = API_BASE_URL;
  }

  initChart();

  // Attach refresh button listener
  if (elements.refreshBtn) {
    elements.refreshBtn.addEventListener('click', handleManualRefresh);
  }

  // Initial fetch
  pollDashboard();

  // Set recurring 3-second polling interval
  pollTimer = setInterval(pollDashboard, POLL_INTERVAL_MS);
});
