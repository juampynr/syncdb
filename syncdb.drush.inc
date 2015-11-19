<?php
/**
 * @file
 * Drush commands to import a large database.
 *
 * Splits the database in a file per table so the import
 * can be made in several processes in parallel.
 */

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
      'source' => 'Site alias referencing the source from which SQL tables will be downloaded.',
    ),
    'required-arguments' => TRUE,
    'options' => array(
      'concurrency' => 'When gnu-parallel is not available, sets the max amount of tables to import in ' .
                       'parallel through drush_invoke_concurrent(). Defaults to 30.',
      'source-dump-dir' => 'The source directory for the dump files.',
      'local-dump-dir' => 'The local destination directory for the dump files.',
    ),
  );

  $items['dumpdb'] = array(
    'bootstrap' => DRUSH_BOOTSTRAP_DRUSH,
    'description' => 'Dumps a database into separate files into a directory.',
    'examples' => array(
      'drush dumpdb' => 'Dumps the current site\'s database into separate files.',
      'drush @example.dev dumpdb' => 'Dumps the Development environment\'s database into separate files.',
    ),
    'options' => array(
      'skip-tables-key' => 'A key in the $skip_tables array. @see example.drushrc.php. Optional.',
      'structure-tables-key' => 'A key in the $structure_tables array. @see example.drushrc.php. Optional.',
      'tables-key' => 'A key in the $tables array. Optional.',
      'skip-tables-list' => 'A comma-separated list of tables to exclude completely. Optional.',
      'structure-tables-list' => 'A comma-separated list of tables to include for structure, but not data. Optional.',
      'tables-list' => 'A comma-separated list of tables to transfer. Optional.',
      'dump-dir' => 'The destination directory for the dump files. If not provided, a temporary directory will be used.',
    ),
  );

  $items['importdb'] = array(
    'bootstrap' => DRUSH_BOOTSTRAP_DRUSH,
    'description' => 'Imports a database from separate dump files in a directory.',
    'examples' => array(
      'drush importdb' => 'Loads the current site\'s database from the current dump files.',
      'drush @example.local importdb' => 'Loads the Development environment\'s database from the current dump files.',
    ),
    'options' => array(
      'concurrency' => 'When gnu-parallel is not available, sets the max amount of tables to import in parallel through drush_invoke_concurrent(). Defaults to 30.',
      'dump-dir' => 'The local directory that holds dump files. If not provided, the temporary directory will be used.',
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
 *   drush @example.local syncdb @example.dev
 */
function drush_syncdb($source) {
  drush_sql_bootstrap_further();

  $source_settings = drush_sitealias_overlay_options(drush_sitealias_get_record($source), 'source-');
  $local_settings = drush_sitealias_overlay_options(drush_sitealias_get_record('@self'), 'local-');

  $source_dump_path = drush_syncdb_dump_dir($source_settings);
  $local_dump_path = drush_syncdb_dump_dir($local_settings);

  // Delete old table files.
  if (is_dir($local_dump_path)) {
    drush_delete_dir($local_dump_path);
  }
  drush_mkdir($local_dump_path);

  // Download dump files to our local temporary directory.
  // @todo If both of these aliases are not remote, then can we avoid the rsync?
  drush_invoke_process('@self', 'core-rsync', array($source . ':' . $source_dump_path, '@self:' . $local_dump_path), array(
    'yes' => TRUE,
    'compress' => TRUE,
  ));
  if (drush_get_error()) {
    return drush_set_error('SYNCDB_RSYNC', dt('Failed to download the database dump files to your local environment.'));
  }

  // Drop all the existing tables in the local environment.
  drush_invoke_process('@self', 'sql-drop', array(), array(
    'yes' => TRUE,
  ));
  if (drush_get_error()) {
    return drush_set_error('SYNCDB_SQL_DROP', dt('Failed to drop existing tables in your local environment.'));
  }

  drush_invoke_process('@self', 'importdb', array(), array(
    'dump-dir' => $local_dump_path,
    'concurrency' => drush_get_option('concurrency', 30),
  ));
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
 * Command callback for drush importdb.
 */
function drush_syncdb_importdb() {
  drush_sql_bootstrap_further();

  // Prepare directory to store database tables.
  $local_settings = drush_sitealias_overlay_options(drush_sitealias_get_record('@self'), '');
  $local_dump_path = drush_syncdb_dump_dir($local_settings);
  if (!is_dir($local_dump_path)) {
    return drush_set_error('SYNCDB_MISSING_DIRECTORY', dt("Directory $local_dump_path does not exist."));
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
 * Implements drush_hook_command().
 *
 * Command callback for dumpdb.
 *
 * Dumps tables from a database into a file per table in the temporary
 * directory. Structure tables are put into structure.sql.
 */
function drush_syncdb_dumpdb() {
  drush_sql_bootstrap_further();

  // Prepare directory to store database tables.
  $local_settings = drush_sitealias_get_record('@self');
  $tables_dir = drush_syncdb_dump_dir($local_settings);
  if (is_dir($tables_dir)) {
    drush_delete_dir($tables_dir);
  }
  drush_mkdir($tables_dir);

  // Extract structure tables into structure.sql.
  if (function_exists('_drush_sql_get_db_spec')) {
    $db_spec = _drush_sql_get_db_spec();
  }
  else {
    $sql = drush_sql_get_class();
    $db_spec = $sql->db_spec();
  }

  if (function_exists('drush_sql_get_expanded_table_selection')) {
    $table_selection = drush_sql_get_expanded_table_selection($db_spec);
  }
  else {
    $table_selection = $sql->get_expanded_table_selection();
  }

  if (empty($table_selection['tables'])) {
    if (function_exists('_drush_sql_get_db_table_list')) {
      $db_tables = _drush_sql_get_db_table_list($db_spec);
    }
    else {
      $db_tables = $sql->listTables();
    }
    $table_selection['tables'] = array_diff($db_tables, $table_selection['structure'], $table_selection['skip']);
  }

  // Build and run the MySQL command and check its results.
  if (!empty($table_selection['structure'])) {
    $structure_tables_file = $tables_dir . '/structure.sql';
    if (function_exists('_drush_sql_get_credentials')) {
      $credentials = _drush_sql_get_credentials($db_spec);
    }
    else {
      $credentials = $sql->creds(FALSE);
    }
    $credentials = str_replace('--database=', ' ', $credentials);
    $exec = _syncdb_build_structure_tables_cmd($db_spec['driver'], $credentials, $table_selection['structure'], $structure_tables_file);
    if (!$exec) {
      return drush_set_error('SYNCDB_UNSPPORTED_DRIVER', dt('The dumpdb command does not support the @driver database driver', array('@driver' => $db_spec['driver'])));
    }
    if (!$return = drush_op_system($exec)) {
      drush_log(dt('Extracted structure tables to !path', array('!path' => $structure_tables_file)), 'success');
      drush_backend_set_result($structure_tables_file);
    }
    else {
      return drush_set_error('SYNCDB_STRUCTURE_DUMP_FAIL', dt('Failed to dump structure tables.'));
    }
  }

  // Extract each table into a file.
  foreach ($table_selection['tables'] as $table) {
    $file_table_selection = array('tables' => array($table), 'skip' => array(), 'structure' => array());
    // Build and run the MySQL command and check its results.
    $file = $tables_dir . '/' . $table . '.sql';
    if (function_exists('drush_sql_build_dump_command')) {
      list($exec, $file) = drush_sql_build_dump_command($file_table_selection, $db_spec, $file);
    }
    else {
      $exec = $sql->dumpCmd($file_table_selection);
      $exec .= ' > ' . $file;
    }
    if (!$return = drush_op_system($exec)) {
      drush_backend_set_result($file);
      drush_log(dt('Extracted table to !path', array('!path' => $file)), 'success');
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
 * @param string $credentials
 *   The database credentials.
 * @param array $tables
 *   The array of structure tables.
 * @param string $file
 *   The file to save the result of the command.
 * @return string
 *   A mysqldump statement that is ready for executing.
 */
function _syncdb_build_structure_tables_cmd($driver, $credentials, $tables, $file) {
  switch ($driver) {
    case 'mysqli':
    case 'mysql':
      $exec = 'mysqldump --result-file ' . $file;
      $exec .= ' --no-autocommit --single-transaction --opt -Q ' . $credentials;
      $exec .= " --no-data " . implode(' ', $tables);
      return $exec;
    case 'pgsql':
      $exec = 'pg_dump --file ' . $file;
      $exec .= str_replace('--dbname=', ' ', $credentials);
      $exec .= " --clean --schema-only " . implode(' ', $tables);
      return $exec;
  }
}

function drush_syncdb_dump_dir($site_record) {
  $databases = sitealias_get_databases_from_record($site_record);
  if (isset($databases)) {
    $db_spec = $databases['default']['default'];
    // Make a base filename pattern to use to name the dump file
    $filename_pattern = $db_spec['database'];
    if (isset($db_spec['remote-host'])) {
      $filename_pattern = $db_spec['remote-host'] . '_' . $filename_pattern;
    }
  }
  // If the user has set the --dump-dir option, then
  // store persistant sql dump files there.
  $dump_dir = drush_sitealias_get_path_option($site_record, 'dump-dir');
  $remote = isset($site_record['remote-host']);
  // If this is a remote site, try to find a writable tmpdir.
  if (empty($dump_dir) && $remote) {
    // If the alias is remote, we'll add on the 'sql' suffix.
    // As mentioned in drush_tempnam, providing a suffix removes
    // the guarentee of tempnam to return a unique filename;
    // however, when the file is going to be used on a remote
    // system, there is no such guarentee anyway, so we might
    // as well include it.
    $remote_site = $site_record;
    unset($remote_site['root']);
    unset($remote_site['uri']);
    $result = drush_invoke_process($site_record, 'php-eval', array('return drush_find_tmp();'), array(), array('integrate' => FALSE, 'override-simulated' => TRUE));
    // If the call to invoke process does not work for some reason
    // (e.g. drush not installed on the target machine),
    // then we will just presume that the tmp dir is '/tmp'.
    if (!$result || !empty($result['error_status']) || empty($result['object'])) {
      $dump_dir = '/tmp/syncdb/' . $filename_pattern;
    }
    else {
      $dump_dir = $result['object'] . '/syncdb/' . $filename_pattern;
    }
  }
  elseif (empty($dump_dir)) {
    $dump_dir = drush_find_tmp() . '/syncdb/' . $filename_pattern;
  }
  return $dump_dir;
}