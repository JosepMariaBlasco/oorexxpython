/* 23-pandas.rex: a pandas DataFrame from CSV text: columns, masks, groupby.
 *
 * Python:
 *   import io, pandas as pd
 *   df = pd.read_csv(io.StringIO("city,month,sales\nGirona,jan,120\n..."))
 *   print(df.shape, list(df.columns))
 *   df["tax"] = df["sales"] * 0.21
 *   df.drop(columns="month", inplace=True)
 *   print(df[df["sales"] > 100])
 *   by = df.groupby("city")["sales"].sum().sort_values(ascending=False)
 *   for city in by.index: print(city.ljust(8), by[city])
 *   print("best:", by.idxmax(), "total:", df["sales"].sum())
 */
pd = .py~import("pandas")
io = .py~import("io")
nl = "0a"x
text = "city,month,sales" || nl ||,
       "Girona,jan,120" || nl || "Girona,feb,150" || nl ||,
       "Lleida,jan,90"  || nl || "Lleida,feb,60"  || nl ||,
       "Reus,jan,200"   || nl || "Reus,feb,180"   || nl
df = pd~read_csv(io~StringIO(text))
say df~shape df~columns~tolist
df["tax"] = df["sales"] * 0.21          -- a new column; Rexx * is pandas' *
df~drop(.py~kw("columns", "month", "inplace", .true))   -- inplace: bool, so True
say df[df["sales"] > 100]               -- a boolean mask selects rows
by = df~groupby("city")["sales"]~sum~sort_values(.py~kw("ascending", .false))
do city over by~index
  say city~left(8) by[city]
end
say "best:" by~idxmax "total:" df["sales"]~sum

::requires "python.cls"
/* Output:
(6, 3) ['city', 'month', 'sales']
     city  sales   tax
0  Girona    120  25.2
1  Girona    150  31.5
4    Reus    200  42.0
5    Reus    180  37.8
Reus     380
Girona   270
Lleida   150
best: Reus total: 800
*/
