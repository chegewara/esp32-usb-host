This new USB host library based is based on changed and hopefully somehow final API from espressif.

The idea is to make this library usable with esp-idf and arduino. I see the need to add files required by arduino and i think this will be maintained in other branch.


For start it is very simple library without proper error handling and probably with many errors, but i decided to share it because i have functional USB MSC code with cool example (functional, not finished). 

Currently library is working only with esp-idf, due to missing some files in arduino. 

Maybe someone would like to join and to add other examples:
- simple read/write example using POSIX
- remote pendrive - we can connect regular pendrive and open files with web browser, there is missing upload files option.
- simple CDC ACM example


Added CDC ACM serial class.
Maybe next step is to add USB HID class with some examples.
