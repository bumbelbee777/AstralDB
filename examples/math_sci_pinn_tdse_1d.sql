-- 1-D time-dependent Schrodinger PINN toy (docs/MathSciPinn.md).
-- MLP activations are stored explicitly to avoid nested scalar builtins in one column.
-- Residual uses finite-difference psi samples at (x,t) and (x+dx,t), (x,t+dt).

DROP TABLE IF EXISTS pinn_tdse_w;
DROP TABLE IF EXISTS pinn_tdse_pts;

CREATE TABLE pinn_tdse_pts (id INT, x TEXT, t TEXT, tag TEXT);
INSERT INTO pinn_tdse_pts VALUES
	(1, '0.50', '0.20', 'center'),
	(2, '0.52', '0.20', 'x_plus'),
	(3, '0.48', '0.20', 'x_minus'),
	(4, '0.50', '0.22', 't_plus');

CREATE TABLE pinn_tdse_w (id INT, w1 TEXT, w2 TEXT);
INSERT INTO pinn_tdse_w VALUES (
	0,
	'T[4,2]:0.5,-0.2, 0.1,0.3, 0.2,0.4, -0.1,0.2',
	'T[2,4]:0.3,0.1,-0.2,0.4, 0.2,-0.1,0.3,0.2'
);

-- Layer 0: linear map from (x,t)
SELECT MATVEC(w1, 'L[2]:0.5,0.2') AS lin_center FROM pinn_tdse_w WHERE id = 0;
SELECT MATVEC(w1, 'L[2]:0.52,0.2') AS lin_x_plus FROM pinn_tdse_w WHERE id = 0;
SELECT MATVEC(w1, 'L[2]:0.48,0.2') AS lin_x_minus FROM pinn_tdse_w WHERE id = 0;
SELECT MATVEC(w1, 'L[2]:0.5,0.22') AS lin_t_plus FROM pinn_tdse_w WHERE id = 0;

-- Hidden activations (sigmoid of precomputed linear outputs)
SELECT SIGMOID('L[4]:0.31,0.52,0.44,0.58') AS h_center;
SELECT SIGMOID('L[4]:0.33,0.51,0.45,0.57') AS h_x_plus;
SELECT SIGMOID('L[4]:0.29,0.53,0.43,0.59') AS h_x_minus;
SELECT SIGMOID('L[4]:0.32,0.54,0.46,0.60') AS h_t_plus;

-- Wavefunction psi = W2 * h (real 2-vector proxy for complex psi)
SELECT MATVEC(w2, 'L[4]:0.31,0.52,0.44,0.58') AS psi_center FROM pinn_tdse_w WHERE id = 0;

-- PDE residual proxy: MSE between time-difference and Laplacian stencil
SELECT
	MSE_LOSS('L[2]:0.008,0.012', 'L[2]:0,0') AS tdse_residual_proxy,
	AD_WIRTINGER_DZ('L[4]:0.10,0.00,0.00,0.00') AS dpsi_dz_sample;

-- Demo training nudge on W2
INSERT INTO pinn_tdse_w VALUES (
	1,
	'T[4,2]:0.5,-0.2, 0.1,0.3, 0.2,0.4, -0.1,0.2',
	'T[2,4]:0.28,0.11,-0.18,0.38, 0.19,-0.09,0.31,0.21'
);

SELECT id, w2 FROM pinn_tdse_w ORDER BY id;

DROP TABLE IF EXISTS pinn_tdse_w;
DROP TABLE IF EXISTS pinn_tdse_pts;
