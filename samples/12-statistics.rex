/* 12-statistics.rex: descriptive statistics with Python's statistics module.
 *
 * Python:
 *   import statistics as st
 *   data = [2, 4, 4, 4, 5, 5, 7, 9]
 *   print(st.mean(data), st.median(data), st.mode(data), st.pstdev(data))
 */
st = .py~import("statistics")
data = 2, 4, 4, 4, 5, 5, 7, 9
say st~mean(data) st~median(data) st~mode(data) st~pstdev(data)

::requires "python.cls"
/* Output:
5 4.5 4 2.0
*/
