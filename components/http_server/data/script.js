const tabs = ["record", "predict", "ota"];
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
      // alert("Recording started for 15 seconds...");
      let progress = 0;
      const interval = setInterval(() => {
        progress += 1;
        document.getElementById("recordProgress").style.width = `${progress}%`;
        if (progress >= 100) clearInterval(interval);
      }, 50);
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

let currentPath = "/";

function refreshFiles(path = currentPath) {
  console.log("The path currently is: ", currentPath);

  fetch(`/list_files?path=${path}`)
    .then((res) => res.json())
    .then((data) => {
      currentPath = data.path;
      document.getElementById("currentPath").textContent = currentPath;
      
      const fileList = document.getElementById("fileList").querySelector("tbody");
      fileList.innerHTML = "";

      // Add parent directory entry (except for root)
      if (currentPath !== "/") {
        const parentRow = document.createElement("tr");
        parentRow.onclick = () => navigateToParent();
        
        parentRow.innerHTML = `
          <td><i class="fas fa-level-up-alt"></i> ..</td>
          <td>Directory</td>
          <td>--</td>
          <td>--</td>
        `;
        fileList.appendChild(parentRow);
      }

      data.files.forEach((file) => {
        const row = document.createElement("tr");
        row.onclick = () => handleFileClick(file);
        
        row.innerHTML = `
          <td><i class="fas ${file.type === 'directory' ? 'fa-folder' : 'fa-file'}"></i> ${file.name}</td>
          <td>${file.type === 'directory' ? "Directory" : "File"}</td>
          <td>${file.type === 'directory' ? "--" : formatFileSize(file.size)}</td>
          <td>${file.modified}</td>
        `;
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

  
function handleFileClick(file) {
    if (file.type === 'directory') {
      refreshFiles(file.path === "/sdcard" ? "/" : file.path.replace("/sdcard", ""));
    } else {
      downloadFile(file.path === "/sdcard" ? "/" : file.path.replace("/sdcard", ""));
    }
}
  
function navigateToParent() {
    const pathParts = currentPath.split('/').filter(Boolean);
    pathParts.pop(); // Remove last part
    const parentPath = pathParts.length ? `/${pathParts.join('/')}` : "/";
    refreshFiles(parentPath);
}
  
function downloadFile(filePath) {
    window.open(`/download_file?path=${encodeURIComponent(filePath)}`, "_blank");
}
  
function formatFileSize(bytes) {
    if (bytes === 0) return "0 Bytes";
    const k = 1024;
    const sizes = ["Bytes", "KB", "MB", "GB"];
    const i = Math.floor(Math.log(bytes) / Math.log(k));
    return parseFloat((bytes / Math.pow(k, i)).toFixed(2) + " " + sizes[i]);
}
  
  // Initialize file manager when Files tab is opened
document.getElementById("tab-files-btn").addEventListener("click", () => {
    refreshFiles(currentPath);
});