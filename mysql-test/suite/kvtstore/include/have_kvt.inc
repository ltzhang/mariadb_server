--disable_query_log
--disable_result_log
--disable_warnings

# Check if KVT plugin exists
let $kvt_exists = `SELECT COUNT(*) FROM INFORMATION_SCHEMA.PLUGINS WHERE PLUGIN_NAME = 'KVT'`;

if (!$kvt_exists)
{
  # Try to check if the plugin file exists
  let $plugin_dir = `SELECT @@plugin_dir`;
  let $kvt_so = `SELECT CONCAT('$plugin_dir', '/ha_kvt.so')`;
  
  # Skip test if KVT is not available
  --skip Test requires KVT storage engine plugin
}

--enable_warnings
--enable_result_log
--enable_query_log