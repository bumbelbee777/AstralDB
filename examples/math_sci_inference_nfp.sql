-- MathSci: MCTS, Bayesian inference, neural macro Fokker–Planck (docs/MathSciInference.md).

DROP TABLE IF EXISTS inf_demo;
CREATE TABLE inf_demo (id INT, tag TEXT, val TEXT);

INSERT INTO inf_demo VALUES (1, 'mcts_best', MCTS_SEARCH('L[4]:0.2,0.5,0.9,0.1', 'L[4]:1,1,1,1', '200', '1.41'));
INSERT INTO inf_demo VALUES (2, 'bayes_beta', BAYES_BETA_POST('1', '1', '7', '3'));
INSERT INTO inf_demo VALUES (3, 'bayes_normal', BAYES_NORMAL_POST('0', '0.01', '0.4', '25', '1'));
INSERT INTO inf_demo VALUES (4, 'log_evidence', BAYES_LOG_EVIDENCE('L[3]:-1,-1,-1', 'L[3]:0,1,2'));

SELECT NFP_MACRO_MARCH(
	'L[16]:0.08,0.09,0.10,0.11,0.10,0.09,0.08,0.07,0.06,0.05,0.04,0.03,0.02,0.01,0.005,0.002',
	DEQ_LINSPACE('0', '4', '16'),
	'L[16]:-0.02,-0.01,0,0.01,0.02,0.02,0.01,0,0,0,0,0,0,0,0,0',
	'L[16]:0.05,0.05,0.05,0.05,0.06,0.06,0.07,0.07,0.08,0.08,0.09,0.09,0.10,0.10,0.11,0.11',
	'L[16]:0,0,0,0,0.001,0.001,0.002,0.002,0,0,0,0,0,0,0,0',
	'2.1',
	'2.0',
	'0',
	'0.05',
	'8'
) AS nfp_g_marched;

SELECT tag, val FROM inf_demo ORDER BY id;

DROP TABLE IF EXISTS inf_demo;
