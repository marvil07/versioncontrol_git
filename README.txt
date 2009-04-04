$Id: README.txt,v 1.1 2008/01/26 21:31:44 boombatower Exp $

Git backend for Version Control API -
Provides Git commit information and account management as a pluggable backend.


SHORT DESCRIPTION
-----------------
This module provides an implementation of the Version Control API that makes
it possible to use the Git version control system. It can retrieve commit
information by parsing commit logs.

For the API documentation, have a look at the module file or run doxygen/phpdoc
on it to get a fancier version of the docs.

Any bug reports or feature requests concerning the Git backend in general
should be submitted to the Git backend issue queue:
http://drupal.org/project/issues/versioncontrol_git.

If you know that the functionality is (or should be) provided by the
Version Control API (and not by the Git backend), please submit an issue there:
http://drupal.org/project/issues/versioncontrol.


AUTHOR
------
Jimmy Berry ("boombatower", http://drupal.org/user/214218)


CREDITS
-------
A good amount of code in Version Control / Project Node Integration was taken
from the CVS backend module on drupal.org, its author (Jakob Petsovits, among others)
deserve a lot of credits and may also hold copyright for parts of this module.
