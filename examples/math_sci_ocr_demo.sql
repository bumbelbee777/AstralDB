-- Minimal vision / OCR-feature pipeline (see docs/MathSciMl.md).
-- Parser note: compose builtins via CTEs; do not nest scalar builtins in one argument.

-- Synthetic 8x8 "stroke" pattern (grayscale).
WITH base AS (
  SELECT 'I[8,8,1]:0,0,0,0,0,0,0,0,0,0,1,1,1,1,0,0,0,1,1,1,1,1,1,0,0,1,1,1,1,1,1,0,0,1,1,1,1,1,1,0,0,0,1,1,1,1,0,0,0,0,0,1,1,0,0,0,0,0,0,0,0,0,0' AS img
),
gray AS (
  SELECT IMG_GRAY(img) AS g FROM base
),
resized AS (
  SELECT IMG_RESIZE(g, '6', '6') AS r FROM gray
)
SELECT
  LIST_LEN(IMG_FLATTEN(r)) AS flat_len,
  LIST_LEN(IMG_HOG_LITE(r, '3')) AS hog_len,
  LIST_LEN(IMG_PATCHES(r, '3', '3', '3')) AS patch_count
FROM resized;

-- Load a tiny 2x2 PGM from hex (H: prefix) via stb_image.
SELECT IMG_GRAY(
  IMG_LOAD('H:50350a3220320a3235350aff80400000')
) AS loaded_gray;
