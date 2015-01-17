<?php
/**
 * @file
 * Drush commands to import a large database.
 *
 * Splits the database in a file per table so the import
 * can be made in several processes in parallel.
 */

// Directory within $TMP to save and download database tables.
define('SYNCDB_TABLES_DIR', 'syncdb-tables');

/**
 * Implements hook_drush_command().
 */
function syncdb_drush_command() {
  $items = array();

  $items['syncdb'] = array(
    'description' => 'Imports a large database.',
    'bootstrap' => DRUSH_BOOTSTRAP_DRUSH,
    'examples' => array(
      'drush syncdb @example.dev' => 'Imports the database from the Development environment. Uses gnu-parallel if available.',
    ),
    'arguments' => array(
      'Source alias' => 'Site alias referencing the source from which SQL tables will be downloaded.',
    ),
    'required-arguments' => TRUE,
    'options' => array(
      'concurrency' => 'When gnu-parallel is not available, sets the max amount of tables to import in ' .
                       'parallel through drush_invoke_concurrent(). Defaults to 30.',
    ),
  );

  $items['dumpdb'] = array(
    'description' => 'Dumps a database into separate files within the temporary directory.',
    'examples' => array(
      'drush dumpdb' => 'Dumps the current site\'s database into separate files.',
      'drush @example.dev dumpdb' => 'Dumps the Development environment\'s database into separate files.',
    ),
    'options' => array(
      'structure-tables-key' => 'Array index of tables whoose data should be ignored ' .
                                'such as cache* tables. See drush topic docs-configuration to find out ' .
                                'where to define this array of tables. Note that if you are running ' .
                                'this command in a remote site through a site alias, the structure-tables-key ' .
                                'array must be available there.',
    ),
  );

  return $items;
}

/**
 * Implements drush_hook_command().
 *
 * Command callback for syncdb.
 *
 * Imports a set of sql files into the current database.
 * Usage:
 *   # Log into the Development environment.
 *   drush @example.dev ssh
 *   # Export database tables into the temporary directory.
 *   drush dumpdb
 *   # Close the SSH connection.
 *   exit
 *   # Import tables into our local environment.
 *   drush @example.dev syncdb
 */
function drush_syncdb($source_alias) {
  // Find out the temporary directory of the source environment.
  $return = drush_invoke_process($source_alias, 'variable-get', array('file_temporary_path'), array(
    'format' => 'list',
  ));
  if ($return['error_status']) {
    return drush_set_error('SYNCDB_SOURCE_TEMP', dt('Failed to obtain the temporary directory from the source alias.'));
  }
  $dev_dump_path = trim($return['output']) . '/' . SYNCDB_TABLES_DIR;
  $local_temp_path = drush_find_tmp() . '/';
  $local_dump_path = $local_temp_path . SYNCDB_TABLES_DIR;

  // Delete old table files.
  if (is_dir($local_dump_path)) {
    drush_delete_dir($local_dump_path);
  }
  mkdir($local_dump_path);

  // Download dump files to our local temporary directory.
  $return = drush_invoke_process('@self', 'core-rsync', array($source_alias . ':' . $dev_dump_path, '@self:' . $local_temp_path), array(
    'yes' => TRUE,
    'compress' => TRUE,
  ));
  if ($return['error_status']) {
    return drush_set_error('SYNCDB_RSYNC', dt('Failed to download the database dump files to your local environment.'));
  }

  // Drop all the existing tables in the local environment.
  $return = drush_invoke_process('@self', 'sql-drop', array(), array(
    'yes' => TRUE,
  ));
  if ($return['error_status']) {
    return drush_set_error('SYNCDB_SQL_DROP', dt('Failed to drop existing tables in your local environment.'));
  }

  // Build a list of the files to import.
  $tables = array();
  foreach (new DirectoryIterator($local_dump_path) as $fileInfo) {
    if (!$fileInfo->isDot()) {
      $tables[] = $fileInfo->getFilename();
    }
  }

  // Decide on how to import SQL files.
  $has_parallel = drush_shell_exec('parallel --version');
  if ($has_parallel) {
    // Woot! GNU-parallel is available.
    _syncdb_parallel_import($local_dump_path, $tables);
  }
  else {
    // Manual import. Still faster than sql-sync.
    drush_log(dt('Importing tables manually. This is still faster than sql-sync but for ludicrous speed install http://www.gnu.org/software/parallel', 'info'), 'status');
    _syncdb_manual_import($local_dump_path, $tables);
  };

  drush_log(dt('Finished importing the database.'), 'success');
}

/**
 * Imports tables using GNU parallel.
 *
 * @param string $local_dump_path
 *   Path where SQL tables where downloaded to.
 * @param string $tables
 *   List of table names within the path.
 */
function _syncdb_parallel_import($local_dump_path) {
  drush_op_system("find " . $local_dump_path . " -name '*sql' | parallel --use-cpus-instead-of-cores --jobs 200% -v drush sql-query --file={}");
}

/**
 * Imports tables manually.
 *
 * @param string $local_dump_path
 *   Path where SQL tables where downloaded to.
 * @param string $tables
 *   List of table names within the path.
 */
function _syncdb_manual_import($local_dump_path, $tables) {
  $concurrency = drush_get_option('concurrency', 30);
  $table_chunks = array_chunk($tables, $concurrency);
  foreach ($table_chunks as $table_chunk) {
    $invocations = array();
    foreach ($table_chunk as $table) {
      $invocations[] = array(
        'site' => '@self',
        'command' => 'sql-query',
        'options' => array(
          'file' => $local_dump_path . '/' . $table,
        ),
      );
    }
    $common_options = array();
    $common_backend_options = array(
      'concurrency' => $concurrency,
    );
    $results = drush_backend_invoke_concurrent($invocations, $common_options, $common_backend_options);

    // Inspect results for this chunk and log them.
    foreach ($results['concurrent'] as $key => $command_log) {
      if (!empty($command_log['error_status'])) {
        return drush_set_error('SYNCDB_TABLE_IMPORT', dt('Failed to import table !table. The error is: !error_log', array(
          '!table' => $table_chunk[$key],
          '!error_log' => print_r($command_log['error_log'], TRUE),
        )));
      }
      else {
        drush_log(dt('Imported table !table', array('!table' => $table_chunk[$key])), 'success');
      }
    }
  }
}

/**
 * Implements drush_hook_command().
 *
 * Command callback for dumpdb.
 *
 * Dumps tables from a database into a file per table in the temporary
 * directory. Structure tables are put into structure.sql.
 */
function drush_syncdb_dumpdb() {
  // Prepare directory to store database tables.
  $local_temp_path = drush_find_tmp() . '/';
  $tables_dir = $local_temp_path . SYNCDB_TABLES_DIR . '/';
  if (is_dir($tables_dir)) {
    drush_delete_dir($tables_dir);
  }
  mkdir($tables_dir);

  // Extract structure tables into structure.sql.
  $db_spec = _drush_sql_get_db_spec();
  $structure_tables_file = $tables_dir . 'structure.sql';

  $table_selection = drush_sql_get_expanded_table_selection($db_spec);
  // Build and run the MySQL command and check its results.
  $exec = _syncdb_build_structure_tables_cmd($db_spec, $table_selection['structure'], $structure_tables_file);
  if (!$return = drush_op_system($exec)) {
    drush_log(dt('Structure tables saved to !path', array('!path' => $structure_tables_file)), 'success');
    drush_backend_set_result($structure_tables_file);
  }
  else {
    return drush_set_error('SYNCDB_STRUCTURE_DUMP_FAIL', dt('Failed to dump structure tables.'));
  }

  // Extract each table into a file.
  $db_tables = _drush_sql_get_db_table_list($db_spec);
  $tables = array_diff($db_tables, $table_selection['structure']);
  $table_selection['skip'] = array();
  $table_selection['structure'] = array();
  foreach ($tables as $table) {
    $table_selection['tables'] = array($table);
    // Build and run the MySQL command and check its results.
    list($exec, $file) = drush_sql_build_dump_command($table_selection, $db_spec, $tables_dir . $table . '.sql');
    if (!$return = drush_op_system($exec)) {
      drush_backend_set_result($file);
      drush_log(dt('Extracted table !path', array('!path' => $file)), 'success');
    }
    else {
      return drush_set_error('SYNCDB_TABLE_DUMP_FAIL', dt('Failed to dump table !path.', array('!path' => $file)));
    }
  }
  drush_log(dt('Finished exporting tables.'), 'success');
}

/**
 * Builds a MySQL command to dump structure tables.
 *
 * @see drush_sql_build_dump_command()
 *
 * drush_sql_build_dump_command() does not allow only structure tables to
 * be dumped so a custom implementation is needed.
 *
 * @param array $db_spec
 *   The database credentials.
 * @param array $tables
 *   The array of structure tables.
 * @param string $file
 *   The file to save the result of the command.
 * @return string
 *   A mysqldump statement that is ready for executing.
 */
function _syncdb_build_structure_tables_cmd($db_spec, $tables, $file) {
  $exec = 'mysqldump --result-file '. $file;
  $extra = ' --no-autocommit --single-transaction --opt -Q' . str_replace('--database=', ' ', _drush_sql_get_credentials($db_spec));
  $exec .= " --no-data $extra " . implode(' ', $tables);
  return $exec;
}
