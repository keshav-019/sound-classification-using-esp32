const tabs = ["record", "predict", "ota", "files"];
tabs.forEach((tab) => {
  document.getElementById(`tab-${tab}-btn`).addEventListener("click", () => {
    tabs.forEach((t) => {
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
    .then((res) => res.json())
    .then((data) => {
      const output = document.getElementById("predictionOutput");
      output.innerText = `Prediction: ${data.category}`;
      output.style.display = "block";
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
    .then((res) => res.text())
    .then((result) => {
      document.getElementById("otaStatus").innerText = result;
    })
    .catch(() => alert("OTA upload failed."));
}

// Add new functions
function refreshFiles() {
  fetch("/list_files")
    .then((res) => res.json())
    .then((data) => {
      const fileList = document
        .getElementById("fileList")
        .querySelector("tbody");
      fileList.innerHTML = "";

      data.files.forEach((file) => {
        const row = document.createElement("tr");

        const nameCell = document.createElement("td");
        nameCell.textContent = file.name;
        if (file.is_dir) {
          nameCell.innerHTML = `<i class="fas fa-folder"></i> ${file.name}`;
        }

        const sizeCell = document.createElement("td");
        sizeCell.textContent = file.is_dir ? "--" : formatFileSize(file.size);

        const actionCell = document.createElement("td");
        if (!file.is_dir) {
          actionCell.innerHTML = `
              <button class="action-button download-btn" onclick="downloadFile('${file.name}')">
                <i class="fas fa-download"></i> Download
              </button>
              <button class="action-button delete-btn" onclick="deleteFile('${file.name}')">
                <i class="fas fa-trash"></i> Delete
              </button>
            `;
        }

        row.appendChild(nameCell);
        row.appendChild(sizeCell);
        row.appendChild(actionCell);
        fileList.appendChild(row);
      });
    })
    .catch(() => alert("Failed to load files"));
}

function formatFileSize(bytes) {
  if (bytes === 0) return "0 Bytes";
  const k = 1024;
  const sizes = ["Bytes", "KB", "MB", "GB"];
  const i = Math.floor(Math.log(bytes) / Math.log(k));
  return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + " " + sizes[i];
}

function downloadFile(filename) {
  window.open(`/download_file?${encodeURIComponent(filename)}`, "_blank");
}

function deleteFile(filename) {
  if (confirm(`Are you sure you want to delete ${filename}?`)) {
    fetch(`/delete_file?${encodeURIComponent(filename)}`)
      .then(() => refreshFiles())
      .catch(() => alert("Failed to delete file"));
  }
}

function deleteAllFiles() {
  if (
    confirm("Are you sure you want to delete ALL files? This cannot be undone!")
  ) {
    fetch("/list_files")
      .then((res) => res.json())
      .then((data) => {
        const deletePromises = data.files
          .filter((file) => !file.is_dir)
          .map((file) =>
            fetch(`/delete_file?${encodeURIComponent(file.name)}`)
          );

        Promise.all(deletePromises)
          .then(() => refreshFiles())
          .catch(() => alert("Some files couldn't be deleted"));
      })
      .catch(() => alert("Failed to load files"));
  }
}

// Refresh files when Files tab is opened
document
  .getElementById("tab-files-btn")
  .addEventListener("click", refreshFiles);
