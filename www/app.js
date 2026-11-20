"use strict";

async function runRequest(button, output, url, options, describe) {
  button.disabled = true;
  output.textContent = "요청 중…";
  try {
    const response = await fetch(url, options);
    const bytes = new Uint8Array(await response.arrayBuffer());
    const text = new TextDecoder().decode(bytes);
    if (!response.ok) throw new Error(`HTTP ${response.status}: ${text.trim()}`);
    output.textContent = describe(text, bytes);
  } catch (error) {
    output.textContent = `요청 실패 · ${error.message}`;
  } finally {
    button.disabled = false;
  }
}

document.querySelector("#echo-form").addEventListener("submit", (event) => {
  event.preventDefault();
  const input = document.querySelector("#message").value;
  const sent = new TextEncoder().encode(input);
  runRequest(document.querySelector("#echo-button"), document.querySelector("#echo-result"),
    "/api/echo", { method: "POST", headers: { "Content-Type": "text/plain; charset=utf-8" }, body: sent },
    (text, received) => {
      const same = sent.length === received.length && sent.every((value, index) => value === received[index]);
      return `${text}\n\n전송 ${sent.length}바이트 · 응답 ${received.length}바이트 · ${same ? "바이트 일치 ✓" : "바이트 불일치"}`;
    });
});

document.querySelector("#time-button").addEventListener("click", () => {
  runRequest(document.querySelector("#time-button"), document.querySelector("#time-result"),
    "/api/time", { cache: "no-store" }, (text) => text);
});
