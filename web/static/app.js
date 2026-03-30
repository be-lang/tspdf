// Generic PDF tool handler
// toolId: string matching the API route
// configFn: function that returns {config: {}, files: {}} from the form
function setupTool(toolId, configFn) {
  const generateBtn = document.getElementById('btn-generate');
  const resultArea = document.getElementById('result-area');
  const errorArea = document.getElementById('error-area');
  const downloadLink = document.getElementById('btn-download');
  const spinner = generateBtn.querySelector('.spinner');

  generateBtn.addEventListener('click', async () => {
    const {config, files} = configFn();

    // Reset state
    resultArea.classList.remove('show');
    errorArea.classList.remove('show');
    generateBtn.disabled = true;
    if (spinner) spinner.style.display = 'block';

    try {
      const formData = new FormData();
      formData.append('config', JSON.stringify(config));
      if (files) {
        for (const [key, file] of Object.entries(files)) {
          if (file) formData.append(key, file);
        }
      }

      const response = await fetch(`/api/${toolId}`, {
        method: 'POST',
        body: formData
      });

      if (!response.ok) {
        const text = await response.text();
        throw new Error(text || response.statusText);
      }

      const blob = await response.blob();
      const url = URL.createObjectURL(blob);
      downloadLink.href = url;
      downloadLink.download = `${toolId}.pdf`;
      resultArea.classList.add('show');

      // Auto-download
      const tmp = document.createElement('a');
      tmp.href = url;
      tmp.download = `${toolId}.pdf`;
      tmp.click();
    } catch (err) {
      errorArea.textContent = err.message;
      errorArea.classList.add('show');
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
