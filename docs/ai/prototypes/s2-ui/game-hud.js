(() => {
  "use strict";

  function number(value, decimals = 1) {
    if (value === null || value === undefined || value === "") return "--";
    const numeric = Number(value);
    return Number.isFinite(numeric) ? numeric.toFixed(decimals) : "--";
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
    const scale = compact ? 1 : 2;
    const margin = compact ? 12 : 24;
    const gap = compact ? 24 : 40;
    const lineHeight = scale * 16 + 4;
    const telemetry = model.telemetry || {};
    const movement = model.movement || {};
    const mode = String(model.mode || "--");
    const deniedMovement = mode.toLowerCase() === "survival";
    const foreground = api.colors.light;
    const outline = "#10212b";
    const hintColor = "#e5d29b";
    const textWidth = (value, textScale = scale) => String(value).length * 8 * textScale;
    const text = (value, x, y, textScale = scale, color = foreground) => {
      api.outlinedText(String(value), Math.round(x), Math.round(y), textScale, color, outline);
    };

    function wrapped(value, x, y, availableWidth, textScale = scale, color = foreground) {
      const columns = Math.max(1, Math.floor(availableWidth / (8 * textScale)));
      const rows = [];
      let remaining = String(value);
      while (remaining.length > columns) {
        const wordEnd = remaining.lastIndexOf(" ", columns);
        const end = wordEnd > 0 ? wordEnd : columns;
        rows.push(remaining.slice(0, end));
        remaining = remaining.slice(end).trimStart();
      }
      if (remaining) rows.push(remaining);
      rows.forEach((row, index) => text(row, x, y + index * (16 * textScale + 4), textScale, color));
      return y + rows.length * (16 * textScale + 4) - 4;
    }

    function fields(rows, x, y, availableWidth) {
      const labelWidth = Math.max(...rows.map(([label]) => label.length)) + 1;
      let bottom = y;
      for (const [label, value] of rows) {
        bottom = wrapped(`${label.padEnd(labelWidth)}${value}`, x, y, availableWidth);
        y = bottom + 4;
      }
      return bottom;
    }

    function flow(values, x, y, availableWidth) {
      let cursor = x;
      let bottom = y;
      for (const value of values) {
        if (cursor > x && cursor + textWidth(value) > x + availableWidth) {
          cursor = x;
          y += lineHeight;
        }
        bottom = Math.max(bottom, wrapped(value, cursor, y, availableWidth));
        cursor += textWidth(value) + (compact ? 16 : 32);
      }
      return bottom;
    }

    const flight = deniedMovement ? "Unavailable" : movement.flight ? "ON" : "OFF";
    const bypass = deniedMovement ? "Unavailable" : movement.bypass ? "ON" : "OFF";
    const footerY = height - (compact ? 80 : 128);
    const footerColumn = Math.floor((width - margin * 2 - gap) / 2);
    const footerRight = margin + footerColumn + gap;

    if (model.visible) {
      const mapLeft = model.minimapRect ? model.minimapRect.x - (compact ? 16 : 24) : width - margin;
      const availableWidth = Math.min(width - margin, mapLeft) - margin;
      const columnWidth = Math.floor((availableWidth - gap) / 2);
      const right = margin + columnWidth + gap;
      const sessionBottom = flow([
        `FPS ${number(telemetry.fps, 0)}`,
        `Uptime ${uptime(telemetry.uptime)}`,
        `Chunks ${number(telemetry.chunks, 0)}`,
        `Memory ${number(telemetry.memoryMiB, 0)} MiB`,
      ], margin, compact ? 8 : 12, availableWidth);
      const headingY = sessionBottom + (compact ? 12 : 24);
      const readoutY = headingY + (compact ? 24 : 44);
      text("Game position example", margin, headingY);
      const positionBottom = fields([
        ["X", number(telemetry.x)],
        ["Z", number(telemetry.z)],
        ["Y", number(telemetry.y)],
        ["YPR deg", `${number(telemetry.yaw)} ${number(telemetry.pitch)} ${number(telemetry.roll)}`],
        ["View", String(model.view || "--")],
      ], margin, readoutY, columnWidth);

      text("Movement", right, headingY);
      const movementBottom = fields([
        ["Effective speed", `${number(model.effectiveSpeed, 0)}x`],
        ["Selected speed", `${number(model.selectedSpeed, 0)}x`],
        ["Acceleration", model.acceleration ? "ON" : "OFF"],
        ["Collision bypass", bypass],
        ["Flight", flight],
      ], right, readoutY, columnWidth);

      const humidity = telemetry.humidity;
      const humidityUnavailable = humidity === null || humidity === undefined
        || humidity === "" || humidity === "--" || humidity === "unavailable";
      const climateY = Math.max(positionBottom, movementBottom) + (compact ? 16 : 24);
      text("Local climate", margin, climateY);
      fields([
        ["Temperature", `${number(telemetry.temperatureC)} C`],
        ["Humidity", humidityUnavailable ? "Unavailable" : String(humidity)],
      ], margin, climateY + (compact ? 24 : 44), columnWidth);
    } else {
      flow([`Flight ${flight}`, `Collision bypass ${bypass}`], margin,
        footerY - (compact ? 32 : 48), width - margin * 2);
    }

    fields([["Selected block", String(model.selectedMaterial || "--")]],
      margin, footerY, footerColumn);
    fields([["Mode", mode]], footerRight, footerY, footerColumn);
    fields([["Break", model.breakRule || (model.canBreak ? "Allowed" : "Unavailable")]],
      margin, footerY + lineHeight, footerColumn);
    fields([["Place", model.placeRule || (model.canPlace ? "Allowed" : "Unavailable")]],
      footerRight, footerY + lineHeight, footerColumn);

    const canEdit = model.canPlace || model.canBreak;
    const hintY = height - (compact ? 40 : 48);
    wrapped([
      canEdit ? "B Change block" : "B Unavailable in Spectator",
      "G Change mode", "M World map", "Esc Leave game",
    ].join("   "), margin, hintY, width - margin * 2, 1, hintColor);
    wrapped([
      model.visible ? "F1 Hide HUD" : "F1 Show HUD",
      "F5/5 Change view",
      deniedMovement ? "F6/6 Unavailable" : "F6/6 Cycle movement",
      "+/- Select speed", "Ctrl Toggle acceleration",
    ].join("   "), margin, hintY + 20, width - margin * 2, 1, hintColor);
  }

  window.S2GameHud = { draw };
})();
