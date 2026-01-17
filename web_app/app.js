import { initializeApp } from "https://www.gstatic.com/firebasejs/9.23.0/firebase-app.js";
import {
  getAuth,
  signInWithEmailAndPassword,
  onAuthStateChanged,
  signOut
} from "https://www.gstatic.com/firebasejs/9.23.0/firebase-auth.js";
import {
  getDatabase,
  ref,
  set,
  onValue,
  remove,
  update
} from "https://www.gstatic.com/firebasejs/9.23.0/firebase-database.js";

// Firebase Configuration
const firebaseConfig = {
  apiKey: "AIzaSyBUtc9tqnY-HiRJ1XhKFyaBlaHuSFNwGaQ",
  authDomain: "fish-feeder-8f8cf.firebaseapp.com",
  databaseURL: "https://fish-feeder-8f8cf-default-rtdb.europe-west1.firebasedatabase.app",
  projectId: "fish-feeder-8f8cf",
  storageBucket: "fish-feeder-8f8cf.firebasestorage.app",
  messagingSenderId: "218952204680",
  appId: "1:218952204680:web:1c3265908f9aa39fdbba3d"
};

// Initialize Firebase
const app = initializeApp(firebaseConfig);
const auth = getAuth();
const db = getDatabase(app);

// UI Elements
const authBox = document.getElementById("authBox");
const controlBox = document.getElementById("controlBox");
const loginBtn = document.getElementById("loginBtn");
const logoutBtn = document.getElementById("logoutBtn");
const authMsg = document.getElementById("authMsg");
const badge = document.getElementById("statusBadge");

// Fish Feeder Elements
const feedNowBtn = document.getElementById("feedNowBtn");
const toggleTimersBtn = document.getElementById("toggleTimersBtn");
const timersSection = document.getElementById("timersSection");
const timersList = document.getElementById("timersList");
const addTimerBtn = document.getElementById("addTimerBtn");
const newTimerInput = document.getElementById("newTimerInput");

// Status Elements
const lastFedTime = document.getElementById("lastFedTime");
const feedCount = document.getElementById("feedCount");
const turbidityValue = document.getElementById("turbidityValue");
const turbidityThreshold = document.getElementById("turbidityThreshold");
const turbidityBar = document.getElementById("turbidityBar");
const turbidityAlert = document.getElementById("turbidityAlert");

// State
let timersVisible = false;
const MAX_TIMERS = 5;

// ==================== NOTIFICATION SYSTEM ====================

function showNotification(message, type = 'warning') {
  // Remove existing notification
  const existing = document.querySelector('.toast-notification');
  if (existing) existing.remove();

  const toast = document.createElement('div');
  toast.className = `toast-notification ${type}`;
  toast.innerHTML = `
    <span class="material-icons">${type === 'error' ? 'error' : type === 'success' ? 'check_circle' : 'warning'}</span>
    <span>${message}</span>
  `;
  document.body.appendChild(toast);

  // Trigger animation
  setTimeout(() => toast.classList.add('show'), 10);

  // Auto-remove after 3 seconds
  setTimeout(() => {
    toast.classList.remove('show');
    setTimeout(() => toast.remove(), 300);
  }, 3000);
}

// ==================== AUTHENTICATION ====================

loginBtn.onclick = async () => {
  authMsg.textContent = "";
  try {
    await signInWithEmailAndPassword(
      auth,
      document.getElementById("emailField").value,
      document.getElementById("passwordField").value
    );
  } catch (e) {
    authMsg.textContent = e.message;
  }
};

logoutBtn.onclick = () => signOut(auth);

onAuthStateChanged(auth, (user) => {
  if (user) {
    authBox.style.display = "none";
    controlBox.style.display = "block";
    badge.style.display = "inline-block"; // Show badge after login
    startListeners();
  } else {
    authBox.style.display = "block";
    controlBox.style.display = "none";
    badge.style.display = "none"; // Hide badge on login page
  }
});

// ==================== FIREBASE LISTENERS ====================

function startListeners() {
  // Device Status - check lastSeen timestamp for accuracy
  onValue(ref(db, "/device"), (snapshot) => {
    const device = snapshot.val();
    if (device) {
      const lastSeen = device.lastSeen ? new Date(device.lastSeen) : null;
      const now = new Date();

      // Device is online if lastSeen is within 2 minutes
      const ONLINE_THRESHOLD_MS = 2 * 60 * 1000; // 2 minutes
      let isOnline = false;

      if (lastSeen && device.online) {
        const timeDiff = now - lastSeen;
        isOnline = timeDiff < ONLINE_THRESHOLD_MS;
      }

      badge.className = `status-badge ${isOnline ? "online" : "offline"}`;
      badge.textContent = isOnline ? "Device Online" : "Device Offline";
    } else {
      badge.className = "status-badge offline";
      badge.textContent = "Device Offline";
    }
  });

  // Feed Count
  onValue(ref(db, "/feedCount"), (snapshot) => {
    const count = snapshot.val();
    feedCount.textContent = count !== null ? count : 0;
  });

  // Last Fed
  onValue(ref(db, "/lastFed"), (snapshot) => {
    const timestamp = snapshot.val();
    if (timestamp) {
      lastFedTime.textContent = formatTimestamp(timestamp);
    } else {
      lastFedTime.textContent = "Never";
    }
  });

  // Turbidity
  onValue(ref(db, "/turbidity"), (snapshot) => {
    const turbidity = snapshot.val();
    if (turbidity) {
      const value = turbidity.value || 0;
      const threshold = turbidity.threshold || 500;
      const alert = turbidity.alert || false;

      turbidityValue.textContent = value;
      turbidityThreshold.textContent = threshold;

      // Update progress bar (cap at 100%)
      const percentage = Math.min((value / threshold) * 100, 100);
      turbidityBar.style.width = `${percentage}%`;

      // Update bar color based on value
      if (value > threshold) {
        turbidityBar.className = "turbidity-bar danger";
      } else if (value > threshold * 0.7) {
        turbidityBar.className = "turbidity-bar warning";
      } else {
        turbidityBar.className = "turbidity-bar";
      }

      // Show/hide alert
      if (alert) {
        turbidityAlert.classList.remove("hidden");
      } else {
        turbidityAlert.classList.add("hidden");
      }
    }
  });

  // Timers
  onValue(ref(db, "/timers"), (snapshot) => {
    const timers = snapshot.val();
    renderTimers(timers);
  });
}

// ==================== FEED NOW ====================

feedNowBtn.onclick = async () => {
  console.log("Feed Now button clicked");
  feedNowBtn.disabled = true;
  feedNowBtn.classList.add("loading");

  try {
    console.log("Attempting to set /feednow to true...");
    await set(ref(db, "/feednow"), true);
    console.log("Feed command sent successfully!");

    // Re-enable after short delay (ESP32 will reset this to false)
    setTimeout(() => {
      feedNowBtn.disabled = false;
      feedNowBtn.classList.remove("loading");
    }, 2000);
  } catch (error) {
    console.error("Error sending feed command:", error);
    alert("Failed to send feed command: " + error.message);
    feedNowBtn.disabled = false;
    feedNowBtn.classList.remove("loading");
  }
};

// ==================== TIMERS ====================

toggleTimersBtn.onclick = () => {
  timersVisible = !timersVisible;
  if (timersVisible) {
    timersSection.classList.remove("hidden");
    toggleTimersBtn.innerHTML = '<span class="material-icons">expand_less</span> Hide Schedule';
  } else {
    timersSection.classList.add("hidden");
    toggleTimersBtn.innerHTML = '<span class="material-icons">alarm</span> Schedule';
  }
};

function renderTimers(timers) {
  timersList.innerHTML = "";

  if (!timers) {
    timersList.innerHTML = '<p class="no-timers">No scheduled feeds</p>';
    return;
  }

  // Sort timers by key (timer0, timer1, timer2)
  const sortedTimers = Object.entries(timers).sort((a, b) => a[0].localeCompare(b[0]));

  sortedTimers.forEach(([key, timer]) => {
    const timerElement = createTimerElement(key, timer);
    timersList.appendChild(timerElement);
  });
}

function createTimerElement(key, timer) {
  const div = document.createElement("div");
  const isTriggered = timer.triggered === true;

  let stateClass = timer.enabled ? "enabled" : "disabled";
  if (isTriggered) stateClass = "triggered";

  div.className = `timer-item ${stateClass}`;
  div.dataset.key = key;

  const timeDisplay = formatTime12Hour(timer.time);

  let statusText = timer.enabled ? "Active" : "Inactive";
  if (isTriggered) statusText = "Completed";

  div.innerHTML = `
    <div class="timer-info">
      <span class="timer-time">${timeDisplay}</span>
      <span class="timer-status">${statusText}</span>
    </div>
    <div class="timer-actions">
      <button class="timer-toggle ${timer.enabled ? "on" : ""}${isTriggered ? " disabled" : ""}" 
              data-key="${key}" 
              data-enabled="${timer.enabled}"
              ${isTriggered ? "disabled" : ""}>
        <span class="material-icons">${timer.enabled ? "toggle_on" : "toggle_off"}</span>
      </button>
      <button class="timer-delete" data-key="${key}">
        <span class="material-icons">delete</span>
      </button>
    </div>
  `;

  // Toggle button (only if not triggered)
  const toggleBtn = div.querySelector(".timer-toggle");
  if (!isTriggered) {
    toggleBtn.onclick = (e) => {
      const btn = e.currentTarget;
      const timerKey = btn.dataset.key;
      const currentEnabled = btn.dataset.enabled === "true";
      update(ref(db, `/timers/${timerKey}`), { enabled: !currentEnabled });
    };
  }

  // Delete button (always available)
  div.querySelector(".timer-delete").onclick = (e) => {
    const timerKey = e.currentTarget.dataset.key;
    if (confirm("Delete this timer?")) {
      remove(ref(db, `/timers/${timerKey}`));
    }
  };

  return div;
}

// Add timer button click handler
addTimerBtn.onclick = async () => {
  const timeValue = newTimerInput.value;
  if (!timeValue) {
    showNotification("Please select a time first", "warning");
    return;
  }

  // Native time input already returns HH:MM format
  const time24 = timeValue;

  try {
    // Get current timers to find next available slot and check limit
    const timersRef = ref(db, "/timers");
    const snapshot = await new Promise((resolve) => {
      onValue(timersRef, resolve, { onlyOnce: true });
    });

    const timers = snapshot.val() || {};
    const timerCount = Object.keys(timers).length;

    // Check if max timers reached
    if (timerCount >= MAX_TIMERS) {
      showNotification(`Maximum ${MAX_TIMERS} timers allowed`, "error");
      return;
    }

    let nextSlot = 0;

    // Find the first available slot (timer0, timer1, timer2, etc.)
    while (timers[`timer${nextSlot}`]) {
      nextSlot++;
    }

    const newTimerKey = `timer${nextSlot}`;
    await set(ref(db, `/timers/${newTimerKey}`), {
      time: time24,
      enabled: true,
      triggered: false
    });

    newTimerInput.value = "";
    showNotification(`Timer added for ${formatTime12Hour(time24)}`, "success");
    console.log(`Timer ${newTimerKey} added: ${time24}`);
  } catch (error) {
    console.error("Error adding timer:", error);
    showNotification("Failed to add timer: " + error.message, "error");
  }
};

// ==================== UTILITY FUNCTIONS ====================

function formatTimestamp(isoString) {
  try {
    const date = new Date(isoString);
    const now = new Date();
    const isToday = date.toDateString() === now.toDateString();

    const timeStr = date.toLocaleTimeString("en-US", {
      hour: "numeric",
      minute: "2-digit",
      hour12: true
    });

    if (isToday) {
      return `Today, ${timeStr}`;
    } else {
      const dateStr = date.toLocaleDateString("en-US", {
        month: "short",
        day: "numeric"
      });
      return `${dateStr}, ${timeStr}`;
    }
  } catch (e) {
    return isoString;
  }
}

function formatTime12Hour(time24) {
  // Convert "HH:MM" to "h:MM AM/PM"
  const [hours, minutes] = time24.split(":");
  const h = parseInt(hours);
  const ampm = h >= 12 ? "PM" : "AM";
  const hour12 = h % 12 || 12;
  return `${hour12}:${minutes} ${ampm}`;
}

function convertTo24Hour(time12) {
  // Handle various formats: "h:mm tt", "hh:mm tt", "h:mm:ss tt"
  const match = time12.match(/(\d{1,2}):(\d{2})(?::\d{2})?\s*(AM|PM|am|pm)/i);
  if (!match) {
    console.log("Could not parse time:", time12);
    return time12; // Return as-is if can't parse
  }

  let [, hours, minutes, period] = match;
  hours = parseInt(hours);

  if (period.toUpperCase() === "PM" && hours !== 12) {
    hours += 12;
  } else if (period.toUpperCase() === "AM" && hours === 12) {
    hours = 0;
  }

  return `${hours.toString().padStart(2, "0")}:${minutes}`;
}