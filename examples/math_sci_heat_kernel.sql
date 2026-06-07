-- Graph and signal heat-kernel builtins (see docs/MathSciCore.md).

-- Triangle graph: nodes 0-1-2; edges as L[2*E] index pairs.
SELECT
  HEAT_KERNEL_GRAPH_STEP('L[3]:1,0,0', 'L[6]:0,1,1,2,0,2', '0.1') AS u_step,
  HEAT_KERNEL_GRAPH_MARCH('L[3]:1,0,0', 'L[6]:0,1,1,2,0,2', '0.05', '8') AS u_march,
  HEAT_KERNEL_GRAPH_APPLY('L[3]:1,0,0', 'L[6]:0,1,1,2,0,2', '0.5') AS u_apply;

-- 1-D Gaussian heat kernel (FFT convolution) and gradient.
SELECT
  HEAT_KERNEL_1D('L[8]:0,0,0,1,1,1,0,0', '1.0') AS smooth_1d,
  HEAT_KERNEL_GRAD_1D('L[8]:0,0,0,1,1,1,0,0', '1.0') AS grad_1d;

-- 2-D heat kernel on image wire I[w,h,c]: (normalized floats).
SELECT HEAT_KERNEL_2D('I[4,4,1]:0,0,0,0,1,1,1,1,1,1,1,1,0,0,0,0', '0.8') AS smooth_2d;
