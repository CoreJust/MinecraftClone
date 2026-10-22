(() => {
  "use strict";

  const nativeColors = {
    cyan: "#7ae0ff",
    gold: "#ffd161",
    green: "#a1ff9e",
    rose: "#ffa6bd",
  };

  function number(value, decimals = 1) {
    if (value === null || value === undefined || value === "") return "--";
    const numeric = Number(value);
    if (!Number.isFinite(numeric)) return "--";
    const fixed = numeric.toFixed(decimals);
    return fixed.length <= 9 ? fixed : numeric.toExponential(2);
  }

  function uptime(value) {
    if (typeof value === "string") return value || "--";
    if (!Number.isFinite(value) || value < 0) return "--";
    const seconds = Math.floor(value);
    return `${String(Math.floor(seconds / 60)).padStart(2, "0")}:${String(seconds % 60).padStart(2, "0")}`;
  }

  function draw(api, model) {
    const { width, height } = api;
    const compact = width < 1050 || height < 520;
    const margin = compact ? 12 : 24;
    const gap = compact ? 24 : 40;
    const colors = {
      light: api.colors.light,
      label: "#d5e0dc",
      quiet: "#c1cfca",
      outline: "#10212b",
      ...nativeColors,
    };
    const telemetry = model.telemetry || {};
    const movement = model.movement || {};
    const mode = String(model.mode || "--");
    const text = (value, x, y, scale = 1, color = colors.light) => {
      api.outlinedText(String(value), Math.round(x), Math.round(y), scale, color, colors.outline);
    };
    const textWidth = (value, scale = 1) => String(value).length * 8 * scale;

    function wrapped(value, x, y, scale, availableWidth, color) {
      const limit = Math.max(1, Math.floor(availableWidth / (8 * scale)));
      let line = "";
      let offset = 0;
      for (const word of String(value).split(/\s+/)) {
        if (line && line.length + 1 + word.length > limit) {
          text(line, x, y + offset, scale, color);
          offset += 16 * scale + 4;
          line = "";
        }
        line += `${line ? " " : ""}${word}`;
      }
      if (line) text(line, x, y + offset, scale, color);
      return offset + 16 * scale;
    }

    function status(label, enabled, x, y, scale = 1) {
      text(label, x, y + (scale - 1) * 12, 1, colors.label);
      text(enabled ? "ON" : "OFF", x + textWidth(label) + 8, y, scale,
        enabled ? colors.green : colors.quiet);
    }

    if (model.visible) {
      const mapLeft = model.minimapRect ? model.minimapRect.x - gap : width - margin;
      const availableWidth = Math.min(width - margin, mapLeft) - margin;
      const positionWidth = Math.floor(availableWidth * (compact ? 0.52 : 0.46));
      const movementX = margin + positionWidth + gap;
      const movementWidth = availableWidth - positionWidth - gap;
      const headingY = compact ? 52 : 64;
      const positionY = compact ? 72 : 88;
      const positionValues = [telemetry.x, telemetry.z, telemetry.y];
      const positionLabels = ["X", "Z", "Y"];
      const positionColors = [colors.cyan, colors.gold, colors.green];
      const cellWidth = Math.floor(positionWidth / 3);

      const fps = number(telemetry.fps, 0);
      const fpsScale = 2;
      text(fps, margin, compact ? 10 : 12, fpsScale);
      text("FPS", margin + textWidth(fps, fpsScale) + 8, compact ? 22 : 24, 1, colors.label);
      if (compact) {
        const sessionX = margin + Math.max(88, textWidth(fps, 2) + 40);
        text(`Uptime ${uptime(telemetry.uptime)}`, sessionX, 10, 1, colors.label);
        text(`Chunks ${number(telemetry.chunks, 0)}  Memory ${number(telemetry.memoryMiB, 0)} MiB`,
          sessionX, 30, 1, colors.label);
      } else {
        let sessionX = margin + Math.max(112, textWidth(fps, fpsScale) + 56);
        for (const value of [
          `Uptime ${uptime(telemetry.uptime)}`,
          `Chunks ${number(telemetry.chunks, 0)}`,
          `Memory ${number(telemetry.memoryMiB, 0)} MiB`,
        ]) {
          text(value, sessionX, 24, 1, colors.label);
          sessionX += textWidth(value) + 24;
        }
      }

      text("GAME POSITION EXAMPLE", margin, headingY, 1, colors.label);
      positionValues.forEach((value, index) => {
        const x = margin + index * cellWidth;
        const formatted = number(value);
        const valueScale = textWidth(formatted, 2) + 20 <= cellWidth ? 2 : 1;
        text(positionLabels[index], x, positionY + 12, 1, positionColors[index]);
        text(formatted, x + 16, positionY + (2 - valueScale) * 12, valueScale, positionColors[index]);
      });

      let orientationX = margin;
      for (const [value, color] of [
        ["YPR", colors.label],
        [number(telemetry.yaw), colors.cyan],
        [number(telemetry.pitch), colors.gold],
        [number(telemetry.roll), colors.rose],
        ["deg", colors.label],
      ]) {
        text(value, orientationX, compact ? 114 : 132, 1, color);
        orientationX += textWidth(value) + 12;
      }

      const viewY = compact ? 136 : 156;
      text("VIEW", margin, viewY + 12, 1, colors.label);
      text(String(model.view || "--"), margin + 48, viewY, 2);
      text("F5/5 Change view", margin + 144, viewY + 12, 1, colors.gold);

      const temperature = `${number(telemetry.temperatureC)} C`;
      const climateY = compact ? 180 : 212;
      text("LOCAL CLIMATE", margin, climateY, 1, colors.label);
      text(temperature, margin, climateY + 20, 2);
      const humidity = telemetry.humidity;
      const humidityUnavailable = humidity === null || humidity === undefined
        || humidity === "" || humidity === "--" || humidity === "unavailable";
      const humidityText = humidityUnavailable ? "Humidity unavailable" : `Humidity ${humidity}`;
      const humidityX = margin + textWidth(temperature, 2) + 16;
      text(humidityText, humidityX, climateY + 32, 1,
        humidityUnavailable ? colors.quiet : colors.light);

      const deniedMovement = mode.toLowerCase() === "survival";
      const movementKey = deniedMovement ? "F6/6 Unavailable" : "F6/6 Cycle";
      const effective = `${number(model.effectiveSpeed, 0)}x`;
      const selected = `${number(model.selectedSpeed, 0)}x`;
      if (compact) {
        const selectedX = movementX + Math.floor((movementWidth + 16) / 2);
        text("EFFECTIVE SPEED", movementX, 10, 1, colors.label);
        text("SELECTED SPEED", selectedX, 10, 1, colors.label);
        text(effective, movementX, 30, 2);
        text(selected, selectedX, 30, 2, colors.gold);
        text("+/- Select speed", movementX, 68, 1, colors.gold);
        status("Acceleration", model.acceleration, movementX, 90);
        text("Ctrl Toggle", movementX + 152, 90, 1, colors.gold);
        if (deniedMovement) {
          text(movementKey, movementX + movementWidth - textWidth(movementKey), 68, 1, colors.quiet);
          text("Flight unavailable (Survival)", movementX, 110, 1, colors.quiet);
          text("Collision bypass unavailable", movementX, 130, 1, colors.quiet);
        } else {
          status("Flight", movement.flight, movementX, 110);
          status("Collision bypass", movement.bypass, movementX + 88, 110);
          text("F6/6 Cycle movement", movementX, 130, 1, colors.gold);
        }
      } else {
        text("MOVEMENT", movementX, 64, 1, colors.label);
        text(movementKey, movementX + movementWidth - textWidth(movementKey), 64,
          1, deniedMovement ? colors.quiet : colors.gold);
        text(effective, movementX, 88, 2);
        text("effective", movementX + textWidth(effective, 2) + 8, 100, 1, colors.label);
        const selectedX = movementX + 224;
        text(selected, selectedX, 88, 2, colors.gold);
        text("selected", selectedX + textWidth(selected, 2) + 8, 100, 1, colors.label);
        text("+/- Select speed", movementX, 132, 1, colors.gold);
        status("Acceleration", model.acceleration, movementX, 156, 2);
        text("Ctrl Toggle", movementX + 184, 168, 1, colors.gold);
        if (deniedMovement) {
          text("Flight unavailable in Survival", movementX, 204, 1, colors.quiet);
          text("Collision bypass unavailable", movementX, 226, 1, colors.quiet);
        } else {
          status("Flight", movement.flight, movementX, 204, 2);
          status("Collision bypass", movement.bypass, movementX + 184, 204, 2);
        }
      }
    }

    const footerY = height - 104;
    const blockWidth = compact ? 176 : 256;
    const modeWidth = compact ? 184 : 256;
    const modeX = margin + blockWidth + gap;
    const rulesX = modeX + modeWidth + gap;
    const hintY = footerY + 60;
    const canEdit = model.canPlace || model.canBreak;
    text("SELECTED BLOCK", margin, footerY, 1, colors.label);
    text(String(model.selectedMaterial || "--"), margin, footerY + 20,
      2, canEdit ? colors.gold : colors.quiet);
    text(canEdit ? "B Change block" : "B Unavailable", margin, hintY, 1,
      canEdit ? colors.gold : colors.quiet);

    text("GAME MODE", modeX, footerY, 1, colors.label);
    text(mode, modeX, footerY + 20, 2);
    text("G Change mode", modeX, hintY, 1, colors.gold);

    text("EDIT RULES", rulesX, footerY, 1, colors.label);
    const ruleScale = 1;
    const rulesWidth = width - margin - rulesX;
    const breakHeight = wrapped(`Break: ${model.breakRule || (model.canBreak ? "Allowed" : "Unavailable")}`,
      rulesX, footerY + 20, ruleScale, rulesWidth,
      model.canBreak ? colors.light : colors.quiet);
    wrapped(`Place: ${model.placeRule || (model.canPlace ? "Allowed" : "Unavailable")}`,
      rulesX, footerY + 20 + breakHeight + 8,
      ruleScale, rulesWidth, model.canPlace ? colors.light : colors.quiet);

    const controlsY = height - 24;
    const controlScale = 1;
    text(model.visible ? "F1 Hide HUD" : "F1 Show HUD", margin, controlsY, controlScale, colors.label);
    text("M World map", margin + 144, controlsY, controlScale, colors.gold);
    text("Esc Leave game", margin + 288, controlsY, controlScale, colors.label);
    if (!model.visible) {
      const capabilities = `Flight ${movement.flight ? "ON" : "OFF"}  Collision bypass ${movement.bypass ? "ON" : "OFF"}`;
      text(capabilities, width - margin - textWidth(capabilities, controlScale), controlsY,
        controlScale, colors.label);
    }
  }

  window.S2GameHud = { draw };
})();
