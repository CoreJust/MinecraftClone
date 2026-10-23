(() => {
  "use strict";

  function draw(api, model) {
    const { context, width, height } = api;
    const compact = width < 960 || height < 520;
    const margin = compact ? 28 : 64;
    const titleScale = compact ? 2 : 3;
    const bodyScale = 2;
    const light = "#f3ead2";
    const accent = "#ffe080";
    const quiet = "#c2d0cf";
    const outline = "#14242d";
    const connecting = model.phase === "connecting";
    const cancelled = Boolean(model.cancelled);
    const title = cancelled
      ? (connecting ? "Connection cancelled" : "Preparation cancelled")
      : (connecting ? "Connecting" : "Preparing to connect");
    const address = String(model.address || "localhost");
    const top = compact ? 28 : Math.round(height * 0.13);
    const titleY = top + (compact ? 24 : 32);
    const serverY = titleY + titleScale * 16 + (compact ? 20 : 28);
    const addressY = serverY + 24;
    const hintY = height - (compact ? 72 : 84);

    context.save();
    context.fillStyle = "#4c718f";
    context.fillRect(0, 0, width, height);
    const sceneAvailable = api.drawCover(api.terrainImage, width, height);
    const shade = context.createLinearGradient(0, 0, 0, height);
    shade.addColorStop(0, "rgba(10, 25, 34, 0.50)");
    shade.addColorStop(0.48, "rgba(10, 25, 34, 0.12)");
    shade.addColorStop(0.68, "rgba(10, 25, 34, 0.04)");
    shade.addColorStop(1, "rgba(10, 25, 34, 0.66)");
    context.fillStyle = shade;
    context.fillRect(0, 0, width, height);

    api.outlinedText(title, margin, titleY, titleScale, light, outline);
    api.outlinedText("SERVER", margin, serverY, 1, quiet, outline);
    api.fitText(address, margin, addressY, bodyScale, width - margin * 2, accent);

    if (cancelled) {
      api.outlinedText("No connection is running", margin,
        addressY + 40, 1, quiet, outline);
    }

    function hint(key, label, x) {
      api.outlinedText(key, x, hintY, bodyScale, accent, outline);
      const labelX = x + (key.length + 1) * 8 * bodyScale;
      api.outlinedText(label, labelX, hintY, bodyScale, light, outline);
      return labelX + label.length * 8 * bodyScale + (compact ? 32 : 48);
    }

    const action = cancelled ? "Retry" : (connecting ? "Generation sample" : "Connect");
    const nextX = hint("ENTER", action, margin);
    hint(cancelled ? "1-4" : "ESC", cancelled ? "Screens" : "Cancel", nextX);
    if (!sceneAvailable) {
      api.outlinedText("Scene capture unavailable", margin, height - 28, 1, quiet, outline);
    }
    context.restore();
  }

  window.S2LoadingView = { draw };
})();
