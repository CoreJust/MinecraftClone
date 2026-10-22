(() => {
  "use strict";

  const MATERIALS = {
    grass: [92, 147, 69],
    stone: [128, 128, 128],
    snow: [244, 247, 250],
    ice: [128, 201, 214],
    air: [189, 217, 237],
  };
  const LOCAL_FRACTION = 0.25;
  const MAX_RASTER_CELLS = 512 * 512;
  let cachedAtlas = null;
  let cachedRaster = null;

  function clamp(value, minimum, maximum) {
    return Math.max(minimum, Math.min(maximum, value));
  }

  function wrap(value, range) {
    return ((value % range) + range) % range;
  }

  function field(atlas, name, count) {
    const values = atlas[name];
    return values && values.length === count ? values : null;
  }

  function prepareRaster(atlas) {
    if (atlas === cachedAtlas) {
      return cachedRaster;
    }
    cachedAtlas = atlas;
    cachedRaster = null;
    if (!atlas || !Number.isInteger(atlas.width) || !Number.isInteger(atlas.height)
      || atlas.width < 2 || atlas.height < 2 || atlas.width * atlas.height > MAX_RASTER_CELLS) {
      return null;
    }

    const { width, height } = atlas;
    const count = width * height;
    const elevation = field(atlas, "elevation", count);
    const temperature = field(atlas, "temperature", count);
    const moisture = field(atlas, "moisture", count);
    const ocean = field(atlas, "oceanMask", count);
    const lake = field(atlas, "lakeMask", count);
    const river = field(atlas, "riverMask", count);
    const waterLevel = field(atlas, "waterLevel", count);
    if (!elevation && !temperature && !ocean && !lake && !river) {
      return null;
    }

    const canvas = document.createElement("canvas");
    canvas.width = width;
    canvas.height = height;
    const rasterContext = canvas.getContext("2d");
    const image = rasterContext.createImageData(width, height);
    for (let y = 0; y < height; y += 1) {
      for (let x = 0; x < width; x += 1) {
        const index = y * width + x;
        const isWater = (ocean && ocean[index]) || (lake && lake[index]) || (river && river[index]);
        const cold = temperature && (isWater ? temperature[index] < -2 : temperature[index] <= -2);
        let material = temperature ? (cold ? MATERIALS.snow : MATERIALS.grass) : MATERIALS.stone;
        let shade = 1;
        if (isWater) {
          material = cold ? MATERIALS.ice : MATERIALS.air;
          const depth = elevation && waterLevel ? Math.max(0, waterLevel[index] - elevation[index]) : 0;
          shade = cold ? 1 : clamp(0.88 - depth * 0.12, 0.53, 0.88);
        } else if (elevation) {
          const west = elevation[y * width + wrap(x - 1, width)];
          const east = elevation[y * width + wrap(x + 1, width)];
          const north = elevation[wrap(y - 1, height) * width + x];
          const south = elevation[wrap(y + 1, height) * width + x];
          shade = clamp(1 + ((west - east) + (north - south)) * 0.5, 0.72, 1.15);
          // Native elevation is geological km; contours mark real 250 m intervals.
          const contour = Math.floor(elevation[index] / 0.25);
          if (contour !== Math.floor(east / 0.25) || contour !== Math.floor(south / 0.25)) {
            shade *= 0.87;
          }
        }
        if (!isWater && !cold && moisture) {
          shade *= 0.96 + clamp(moisture[index], 0, 1) * 0.08;
        }
        const pixel = index * 4;
        for (let channel = 0; channel < 3; channel += 1) {
          image.data[pixel + channel] = clamp(Math.round(material[channel] * shade), 0, 255);
        }
        image.data[pixel + 3] = 255;
      }
    }
    rasterContext.putImageData(image, 0, 0);
    cachedRaster = {
      canvas,
      caption: !temperature ? "No climate data" : !elevation ? "No relief data"
        : !ocean && !lake && !river ? "No water data" : "Climate proxy",
    };
    return cachedRaster;
  }

  function drawLocalRaster(context, raster, player, x, y, size) {
    const sourceWidth = raster.width * LOCAL_FRACTION;
    const sourceHeight = raster.height * LOCAL_FRACTION;
    const sourceX = wrap(player.x, 1) * raster.width - sourceWidth / 2;
    const sourceY = wrap(player.y, 1) * raster.height - sourceHeight / 2;
    context.save();
    context.beginPath();
    context.rect(x, y, size, size);
    context.clip();
    context.imageSmoothingEnabled = false;
    for (let row = -1; row <= 1; row += 1) {
      for (let column = -1; column <= 1; column += 1) {
        context.drawImage(raster,
          x + (column * raster.width - sourceX) * size / sourceWidth,
          y + (row * raster.height - sourceY) * size / sourceHeight,
          raster.width * size / sourceWidth,
          raster.height * size / sourceHeight);
      }
    }
    context.restore();
  }

  function drawMarker(context, x, y, heading, compact, colors) {
    context.save();
    context.translate(x, y);
    context.rotate(heading * Math.PI / 180);
    const reach = compact ? 9 : 11;
    context.beginPath();
    context.moveTo(0, -reach);
    context.lineTo(reach * 0.65, reach * 0.7);
    context.lineTo(0, reach * 0.35);
    context.lineTo(-reach * 0.65, reach * 0.7);
    context.closePath();
    context.lineWidth = 4;
    context.strokeStyle = colors.dark;
    context.stroke();
    context.fillStyle = colors.accent;
    context.fill();
    context.restore();
  }

  function draw(api, { player }) {
    const { context, width, height, colors } = api;
    const size = Math.max(128, Math.floor(Math.min(192, height * 0.35, width * 0.19) / 16) * 16);
    const x = Math.floor(width - size - 34);
    const y = 44;
    const centerX = x + size / 2;
    const centerY = y + size / 2;
    const heading = wrap(Number.isFinite(player.heading) ? player.heading : 0, 360);
    const raster = prepareRaster(api.atlas === undefined ? window.S2_WORLD_DATA : api.atlas);

    context.save();
    context.fillStyle = colors.dark;
    context.fillRect(x - 3, y - 3, size + 6, size + 6);
    if (raster) {
      drawLocalRaster(context, raster.canvas, player, x, y, size);
      drawMarker(context, centerX, centerY, heading, size < 160, colors);
      const caption = raster.caption;
      context.fillStyle = "rgba(16, 33, 43, 0.83)";
      context.fillRect(x, y + size - 20, size, 20);
      api.text(caption, x + Math.max(4, (size - caption.length * 8) / 2), y + size - 18, 1, colors.light);
    } else {
      api.text("Map unavailable", x + Math.max(4, (size - 15 * 8) / 2), centerY - 20, 1, colors.light);
      api.text("No native data", x + Math.max(4, (size - 14 * 8) / 2), centerY, 1, colors.quiet);
    }
    context.strokeStyle = colors.light;
    context.lineWidth = 1;
    context.strokeRect(x - 0.5, y - 0.5, size + 1, size + 1);
    api.outlinedText("N", centerX - 8, y - 36, 2, colors.accent);
    api.outlinedText("W", x - 24, centerY - 16, 2, colors.light);
    api.outlinedText("E", x + size + 8, centerY - 16, 2, colors.light);
    api.outlinedText("S", centerX - 8, y + size + 4, 2, colors.light);
    api.outlinedText(`${String(Math.round(heading) % 360).padStart(3, "0")}deg`,
      x + size - 48, y - 28, 1, colors.light);
    context.restore();
    return { x: x - 26, y: y - 38, width: size + 52, height: size + 76 };
  }

  window.S2MinimapView = { draw };
})();
