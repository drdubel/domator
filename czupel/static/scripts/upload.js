const form = document.getElementById("uploadForm");
const statusDiv = document.getElementById("status");

Sentry.init({
    dsn: SENTRY_DSN,
    // Setting this option to true will send default PII data to Sentry.
    // For example, automatic IP address collection on events
    integrations: [Sentry.browserTracingIntegration()],
    tracesSampleRate: 1.0,
    sendDefaultPii: true,
    tracePropagationTargets: ["localhost", /^https:\/\/czupel\.dry\.pl\//],
});

form.addEventListener("submit", async (e) => {
    e.preventDefault();
    statusDiv.textContent = "Uploading...";

    const formData = new FormData();

    let device = document.getElementById("device").value;
    device = device.trim().toLowerCase().replace(/\s+/g, "_");

    const file = document.getElementById("firmware").files[0];

    if (!device || !file) {
        statusDiv.textContent = "Please select device and firmware.bin";
        return;
    }

    formData.append("file", file);

    try {
        const res = await fetch(`/upload/${device}`, {
            method: "POST",
            body: formData,
            credentials: "include"  // send cookies for auth
        });

        if (res.ok) {
            statusDiv.textContent = "✅ Upload successful!";
        } else {
            statusDiv.textContent = `❌ Upload failed (status ${res.status})`;
        }
    } catch (err) {
        statusDiv.textContent = "⚠️ Error uploading: " + err.message;
    }
});