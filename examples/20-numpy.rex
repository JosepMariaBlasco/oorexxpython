/* 20-numpy.rex: numpy arrays: Rexx operators are numpy's.
 *
 * Python:
 *   import numpy as np
 *   a = np.array([1, 2, 3, 4])
 *   print(a * 10, a.mean(), a[a > 2])
 *   m = np.array([[1, 2], [3, 4]])
 *   print(m @ m, m[1, 0], np.linalg.det(m).round(6))
 */
np = .py~import("numpy")
a = np~array(.array~of(1, 2, 3, 4))
say a * 10 a~mean a[a > 2]
m = np~array(.array~of(.array~of(1, 2), .array~of(3, 4)))
say np~matmul(m, m)~tolist m[1, 0] np~round(np~linalg~det(m), 6)

::requires "python.cls"
/* Output:
[10 20 30 40] 2.5 [3 4]
[[7, 10], [15, 22]] 3 -2.0
*/
