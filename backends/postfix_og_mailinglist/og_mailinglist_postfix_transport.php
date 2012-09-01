#!/usr/bin/php
<?php

// Reads in raw email off the STDIN and posts the email using our curl command
// for the appropriate site.

$raw_email = '';
// Grab the raw email message from stdin.
$fd = fopen("php://stdin", "r");
while (!feof($fd)) {
  $raw_email .= fread($fd, 1024);
}

// Set command line arguments (sent by the postfix transport) to variables.
$parts = explode("@", $argv[1]);
$mail_username = $parts[0];
$mail_domain = strtolower($parts[1]);

// Load site info
require_once('site_info.php');
$sites = og_mailinglist_site_info();

if (empty($sites[$mail_domain])) {
  echo "Could not match the email domain $mail_domain with a Drupal site. Check that you've setup site_info.php correctly.";
  exit();
}

$post_url = $sites[$mail_domain]['post_url'];
$validation_string = $sites[$mail_domain]['validation_string'];
$token = md5($validation_string . $raw_email);

$ch = curl_init();
curl_setopt($ch, CURLOPT_RETURNTRANSFER, FALSE);
curl_setopt($ch, CURLOPT_HTTPHEADER, array('Expect:'));
curl_setopt($ch, CURLOPT_POST, 1);
curl_setopt($ch, CURLOPT_HEADER, 1);
curl_setopt($ch, CURLOPT_URL, $post_url);

//prepare the field values being posted to the service
$data = array(
  'message' => $raw_email,
  'token' => $token,
  'group_name' => $mail_username,
);

curl_setopt($ch, CURLOPT_POSTFIELDS, $data);

//make the request
curl_exec($ch);
