DESCRIPTION
===========

This project implements two Drush commands to export and import large
databases. By splitting tables into separate files and importing them
afterwards in parallel we can reduce the overall processing time
considerably.

There is no .module nor .info files because this is not a module. It is a Drush
command. Drush can find commands in certain directories such as $HOME/.drush/
or sites/all/drush. Run `drush topic docs-commands` on a terminal to see other
places where this project can be placed so Drush can discover it. Depending on
your needs you may decide to leave this command with or without your project's
versioned code.

REQUIREMENTS
============

This command works on Drush 6 or higher. It has been tested only in MySQL.

It highly recommended that
you install GNU-parallel (http://www.gnu.org/software/parallel) in the
machine(s) where you will import tables.

On Ubuntu, run sudo apt-get install parallel.
On Mac, run brew install parallel.

drush syncdb will still be able to import tables witout GNU-parallel, but it
will take longer to complete.

INSTALLATION
============

Go to your project's root directory and run the following command:

$ drush dl syncdb

This will normally download the command into `/path-to-drush/drush/commands/syncdb`.
If you want to move it somewhere within your project so it is under version control,
move it to sites/all/drush or run drush dl --destination=sites/all/drush syncdb.

Next, log into the remote server which will server as the source from which
the team will download tables into their local environments. Install the command
there.

USAGE
=====

You should set up a periodic job that runs drush dumpdb at the remote environment
that is designated as the source. This would normally be the Development
environment. This can be set up through crontab or Jenkins. Here is how you
can set this up in crontab:

drush @example.dev ssh
crontab -e
# Paste the following command at the bottom of the opened file:
30 2 * * * drush --root=/path/to/your/drupalroot --quiet dumpdb

Once this job is set up, run it once in the remote environment so it will export
tables into, for example, /tmp/syncdb-tables. Now you can import these tables
into your local environment with the following command:

drush syncdb @example.dev

CUSTOMIZING
===========

You can use --structure-tables-key option in the same way it works at sql-sync. This
option will export structure tables into an extra file called structure.sql.

If you install parallel, have a look at is options by reading the contents of the
command "man parallel". There could be ways for you to optimize the command even further.

ACKNOWLEDGEMENTS
================

* Andrew Berry (deviantintegral) for creating https://github.com/deviantintegral/mysql-parallel
  where I took some of the ideas.
* Mateu Aguiló Bosh (e0ipso) for showing me how mysql-parallel works.
* Dave Reid, for writing https://www.drupal.org/project/concurrent_queue, where I took the
  drush_invoke_concurrent() approach when GNU-parallel is not available.
* Kris Bulman (krisbulman), for reminding me every week that sql-sync was slow as hell.
