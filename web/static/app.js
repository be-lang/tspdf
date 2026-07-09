function setOperationStatus(message) {
  const statusEl = document.getElementById('operation-status');
  if (statusEl) statusEl.textContent = message;
}

function buildActionableError(errMessage, statusCode) {
  const msg = (errMessage || '').trim();
  let tip = '';

  if (statusCode === 408 || /timeout/i.test(msg)) {
    tip = 'Tip: Retry with a smaller file set or check local CPU load.';
  } else if (statusCode === 400 || /missing|invalid|bad request/i.test(msg)) {
    tip = 'Tip: Check that required files and options are provided and valid.';
  } else if (statusCode >= 500 || /failed|overflow|out of memory/i.test(msg)) {
    tip = 'Tip: Try a smaller or less complex file, then retry.';
  } else {
    tip = 'Tip: Check that the file is a valid PDF (or supported image/markdown input) and retry.';
  }

  return msg ? `${msg} ${tip}` : tip;
}

// Backend seam. The served UI posts to the local tspdf server; the static
// wasm build (see wasm/demo/) assigns window.TSPDF_BACKEND before any click
// to run everything in-browser instead. Both paths return a fetch Response.
async function backendRun(toolId, config, files) {
  if (window.TSPDF_BACKEND) return window.TSPDF_BACKEND.run(toolId, config, files);
  const formData = new FormData();
  formData.append('config', JSON.stringify(config));
  if (files) {
    for (const [key, file] of Object.entries(files)) {
      if (file) formData.append(key, file);
    }
  }
  return fetch(`/api/${toolId}`, {
    method: 'POST',
    body: formData
  });
}

// Generic PDF tool handler
// toolId: string matching the API route
// configFn: function that returns {config: {}, files: {}}
function setupTool(toolId, configFn) {
  const generateBtn = document.getElementById('btn-generate');
  const resultArea = document.getElementById('result-area');
  const errorArea = document.getElementById('error-area');
  const downloadLink = document.getElementById('btn-download');
  const spinner = generateBtn.querySelector('.spinner');
  let previousBlobUrl = null;

  generateBtn.addEventListener('click', async () => {
    const {config, files} = configFn();

    // Reset state
    resultArea.classList.remove('show');
    errorArea.classList.remove('show');
    generateBtn.disabled = true;
    if (spinner) spinner.style.display = 'block';
    setOperationStatus('Processing locally... Keep this tab open.');

    try {
      const response = await backendRun(toolId, config, files);

      if (!response.ok) {
        const text = await response.text();
        throw new Error(buildActionableError(text || response.statusText, response.status));
      }

      const blob = await response.blob();
      const url = URL.createObjectURL(blob);
      if (previousBlobUrl) URL.revokeObjectURL(previousBlobUrl);
      previousBlobUrl = url;
      downloadLink.href = url;
      downloadLink.download = `${toolId}.pdf`;
      resultArea.classList.add('show');
      setOperationStatus('Done. Your file is ready to download.');

      // Auto-download
      const tmp = document.createElement('a');
      tmp.href = url;
      tmp.download = `${toolId}.pdf`;
      tmp.click();
    } catch (err) {
      errorArea.textContent = err.message || buildActionableError('', 0);
      errorArea.classList.add('show');
      setOperationStatus('Operation failed locally. See the error and tip above, then retry.');
    } finally {
      generateBtn.disabled = false;
      if (spinner) spinner.style.display = 'none';
    }
  });
}

// Drag-and-drop enhancement for drop zones
document.querySelectorAll('.drop-zone').forEach(zone => {
  zone.addEventListener('dragover', e => { e.preventDefault(); zone.classList.add('drag-over'); });
  zone.addEventListener('dragleave', () => zone.classList.remove('drag-over'));
  zone.addEventListener('drop', e => {
    e.preventDefault();
    zone.classList.remove('drag-over');
    const input = zone.querySelector('input[type=file]');
    if (input && e.dataTransfer.files.length) {
      input.files = e.dataTransfer.files;
      input.dispatchEvent(new Event('change'));
    }
  });
});

// Show selected filename(s) in drop zone
document.querySelectorAll('.drop-zone input[type=file]').forEach(input => {
  input.addEventListener('change', function() {
    const zone = this.closest('.drop-zone');
    const mainText = zone.querySelector('p');
    const hint = zone.querySelector('.dz-hint');
    if (this.files.length === 1) {
      if (mainText) mainText.textContent = '\u2713 ' + this.files[0].name;
      if (hint) hint.textContent = this.files[0].name.length > 40 ?
        this.files[0].name.substring(0, 37) + '...' : '';
      if (mainText) mainText.style.color = '#4ade80';
    } else if (this.files.length > 1) {
      if (mainText) mainText.textContent = '\u2713 ' + this.files.length + ' files selected';
      if (hint) hint.textContent = Array.from(this.files).map(f => f.name).join(', ');
      if (mainText) mainText.style.color = '#4ade80';
    }
  });
});
