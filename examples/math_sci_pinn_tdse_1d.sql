-- 1-D time-dependent Schrodinger PINN toy (docs/MathSciPinn.md).
-- Uses PREDICT(), MATHSCI_MODEL_* binary I/O, and enriched autograd builtins.

DROP TABLE IF EXISTS pinn_tdse_model;
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

-- Binary model from weight tables
CREATE TABLE pinn_tdse_model (id INT, mdl TEXT);
INSERT INTO pinn_tdse_model VALUES (
	0,
	MATHSCI_MODEL_BUILD(
		'L[2]:sigmoid,linear',
		'LW[2]:T[4,2]:0.5,-0.2, 0.1,0.3, 0.2,0.4, -0.1,0.2;T[2,4]:0.3,0.1,-0.2,0.4, 0.2,-0.1,0.3,0.2'
	)
);

SELECT PREDICT(mdl, 'L[2]:0.5,0.2') AS psi_center FROM pinn_tdse_model WHERE id = 0;
SELECT MATHSCI_MODEL_SERIALIZE(mdl) AS model_blob FROM pinn_tdse_model WHERE id = 0;

-- Forward via MATVEC + vectorized SIGMOID (split columns; no nested builtins)
SELECT MATVEC(w1, 'L[2]:0.5,0.2') AS lin_center FROM pinn_tdse_w WHERE id = 0;
SELECT SIGMOID('L[4]:0.31,0.52,0.44,0.58') AS h_center;
SELECT MATVEC(w2, 'L[4]:0.31,0.52,0.44,0.58') AS psi_matvec FROM pinn_tdse_w WHERE id = 0;

-- Finite-difference stencil + Wirtinger sample
SELECT
	MSE_LOSS('L[2]:0.008,0.012', 'L[2]:0,0') AS tdse_residual_proxy,
	PINN_FD_CENTRAL('0.33', '0.29', '0.02') AS dpsi_dx_fd,
	AD_WIRTINGER_DZ('L[4]:0.10,0.00,0.00,0.00') AS dpsi_dz_sample;

-- Autograd: MSE residual gradient w.r.t. prediction
SELECT AD_GRAD_MSE_PRED(
	PREDICT((SELECT mdl FROM pinn_tdse_model WHERE id = 0), 'L[2]:0.5,0.2'),
	'L[2]:0,0'
) AS grad_psi;

-- Toy training nudge on W2
INSERT INTO pinn_tdse_w VALUES (
	1,
	'T[4,2]:0.5,-0.2, 0.1,0.3, 0.2,0.4, -0.1,0.2',
	'T[2,4]:0.28,0.11,-0.18,0.38, 0.19,-0.09,0.31,0.21'
);

INSERT INTO pinn_tdse_model VALUES (
	1,
	MATHSCI_MODEL_IMPORT(
		MATHSCI_MODEL_SERIALIZE(
			MATHSCI_MODEL_BUILD(
				'L[2]:sigmoid,linear',
				'LW[2]:T[4,2]:0.5,-0.2, 0.1,0.3, 0.2,0.4, -0.1,0.2;T[2,4]:0.28,0.11,-0.18,0.38, 0.19,-0.09,0.31,0.21'
			)
		)
	)
);

SELECT id, MATHSCI_MODEL_FINGERPRINT(mdl) AS fp FROM pinn_tdse_model ORDER BY id;

DROP TABLE IF EXISTS pinn_tdse_model;
DROP TABLE IF EXISTS pinn_tdse_w;
DROP TABLE IF EXISTS pinn_tdse_pts;
