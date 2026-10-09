/* 16-uuid.rex: UUIDs with Python's uuid module (uuid5 is name-based, so the
 * same name always gives the same UUID; uuid4 is random).
 *
 * Python:
 *   import uuid
 *   u = uuid.uuid5(uuid.NAMESPACE_DNS, "rexxla.org")
 *   print(u, u.version)
 */
uuid = .py~import("uuid")
u = uuid~uuid5(uuid~NAMESPACE_DNS, "rexxla.org")
say u u~version
say uuid~uuid4~string~length "characters"

::requires "python.cls"
/* Output:
5973c46b-ea41-5d9c-a014-8c26c0195f56 5
36 characters
*/
