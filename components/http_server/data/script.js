const tabs = ["record", "predict", "ota"];
tabs.forEach(tab => {
    document.getElementById(`tab-${tab}-btn`).addEventListener("click", () => {
        tabs.forEach(t => {
            document.getElementById(`tab-${t}`).classList.remove("active");
            document.getElementById(`tab-${t}-btn`).classList.remove("active");
        });
        document.getElementById(`tab-${tab}`).classList.add("active");
        document.getElementById(`tab-${tab}-btn`).classList.add("active");
    });
});

function startRecording() {
    const category = document.getElementById("categorySelect").value;
    fetch(`/start_recording?category=${category}`)
    .then(() => {
    alert("Recording started for 15 seconds...");
    let progress = 0;
    const interval = setInterval(() => {
        progress += 1;
        document.getElementById("recordProgress").style.width = `${progress}%`;
        if (progress >= 100) clearInterval(interval);
    }, 150);
    })
    .catch(() => alert("Failed to start recording."));
}

function stopRecording() {
    fetch(`/stop_recording`)
    .then(() => alert("Recording stopped."))
    .catch(() => alert("Failed to stop recording."));
}

function predictSound() {
    fetch("/predict")
    .then(res => res.json())
    .then(data => {
    const output = document.getElementById("predictionOutput");
    output.innerText = `Prediction: ${data.category}`;
    output.style.display = 'block';
    })
    .catch(() => alert("Prediction failed."));
}

function uploadOTA() {
    const fileInput = document.getElementById("firmwareFile");
    const file = fileInput.files[0];
    if (!file) {
        alert("No file selected.");
        return;
    }

    const formData = new FormData();
    formData.append("firmware", file);

    fetch("/update", {
        method: "POST",
        body: formData,
    })
    .then(res => res.text())
    .then(result => {
    document.getElementById("otaStatus").innerText = result;
    })
    .catch(() => alert("OTA upload failed."));
}