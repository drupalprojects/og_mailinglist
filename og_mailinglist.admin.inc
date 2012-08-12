<?php
/**
 * @file
 *  Admin interface for OG Mailinglist module.
 */

/**
 * Admin settings form for email settings
 */
function og_mailinglist_email_settings() {
  $form['og_mailinglist_max_message_size'] = array(
    '#title' => t('Maximum message body size (excluding attachments) (in Kb)'),
    '#type' => 'textfield',
    '#default_value' => variable_get('og_mailinglist_max_message_size', '100'),
    '#description' => t('Any messages over this size will be rejected.  Set to 0 for unlimited.'),
  );
  $form['og_mailinglist_max_posts_per_hour'] = array(
    '#title' => t('Maximum posts per hour'),
    '#type' => 'textfield',
    '#default_value' => variable_get('og_mailinglist_max_posts_per_hour', '20'),
    '#description' => t('The maximum number of messages a user is able to post per hour by email.  Set to 0 for unlimited.'),
  );
  $form['og_mailinglist_reply_to_group'] = array(
    '#title' => t('Add a Reply-To header to outgoing group emails'),
    '#type' => 'checkbox',
    '#default_value' => variable_get('og_mailinglist_reply_to_group', '0'),
    '#description' => t('Set an explicit reply-to header. See http://wiki.list.org/pages/viewpage.action?pageId=4030691 for pros and cons on this setting.'),
  );
  $form['og_mailinglist_thread_comments'] = array(
    '#title' => t('Thread comments, using In-Reply-To header'),
    '#type' => 'checkbox',
    '#default_value' => variable_get('og_mailinglist_thread_comments', '0'),
    '#description' => t('New comments will be threaded on save. Existing comments will not be affected.'),
  );
  $form['og_mailinglist_add_signatures'] = array(
    '#title' => t('Add user signatures to nodes and comments created on the site'),
    '#type' => 'checkbox',
    '#default_value' => variable_get('og_mailinglist_add_signatures', '1'),
    '#description' => t('Add a user\'s signature if they have one.'),
  );
  return system_settings_form($form);
}

/**
 * Admin settings form for OGM content types
 */
function og_mailinglist_content_types_settings() {
  $node_types = _node_types_build()->types;
  $n_types = array();
  foreach ($node_types as $type) {
    $n_types[$type->type] = $type->name;
  }
  $form['og_mailinglist_default_content_type'] = array(
    '#title' => t('Default content type'),
    '#type' => 'select',
    '#default_value' => variable_get('og_mailinglist_default_content_type', 'story'),
    '#options' => $n_types,
    '#description' => t('Choose the default content type to be created from new emails.'),
  );
  $form['og_mailinglist_exclude_content_types'] = array(
    '#title' => t('Exclude the following content types'),
    '#type' => 'select',
    '#multiple' => TRUE,
    '#default_value' => variable_get('og_mailinglist_exclude_content_types', 'group'),
    '#options' => $n_types,
    '#description' => t('Do not send emails on creation of these content types.'),
  );
  $form['og_mailinglist_default_group_subscription_type'] = array(
    '#title' => t('Default group email subscription type'),
    '#type' => 'select',
    '#default_value' => variable_get('og_mailinglist_default_group_subscription_type', 'email'),
    '#options' => array(
      'email' => t('Email'),
      'digest email' => t('Digest email'),
      'no email' => t('No email'),
    ),
    '#description' => t('Choose the default email subscription for users joining a new group. "Email" means the user will receive an email for each new post. "No email" means the user will recieve no emails except for discussion threads they participate in. "Digest" is the same as "no email" except the user will also receive digest emails. (Users can change this later at http://example.org/user/%user/og_mailinglist)'),
  );
  return system_settings_form($form);
}

/**
 * Admin settings form for digest emails.
 */
function og_mailinglist_digest_settings() {
  $digest_days = variable_get('og_mailinglist_digest_interval_days', 1);
  $digest_hour = variable_get('og_mailinglist_digest_hour', 17);
  $next_digest = variable_get('og_mailinglist_digest_runtime',
    mktime($digest_hour, 0, 0));
  variable_set('og_mailinglist_digest_runtime', $next_digest);

  $form['og_mailinglist_digest_interval_days'] = array(
    '#title' => t('Digest interval'),
    '#type' => 'textfield',
    '#default_value' => $digest_days,
    '#size' => 5,
    '#maxlength' => 3,
    '#required' => TRUE,
    '#description' => t('Choose how often the digest will be sent. Value is in days. Defaults to 1.'),
  );
  $form['og_mailinglist_digest_hour'] = array(
    '#title' => t('Digest hour'),
    '#type' => 'textfield',
    '#size' => 5,
    '#maxlength' => 2,
    '#required' => TRUE,
    '#default_value' => $digest_hour,
    '#description' => t('The digest email will be sent after this hour. Defaults to 17.'),
  );
  $form['og_mailinglist_digest_next'] = array(
    '#title' => t('Next digest run'),
    '#type' => 'item',
    '#description' => t('Next digest is scheduled to @datestring. Please keep in mind that the digest mail will be sent on the next cron run after this date, so it depends on cron settings.',
      array('@datestring' => format_date($next_digest))),
  );
  return system_settings_form($form);
}

/**
 * Validate admin digest emails settings form.
 */
function og_mailinglist_digest_settings_validate($form, &$form_state) {
  $days = $form_state['values']['og_mailinglist_digest_interval_days'];
  if (!is_numeric($days)) {
    form_set_error('og_mailinglist_digest_interval_days',
      t('Digest interval must be a number.'));
  }
  elseif ($days <= 0) {
    form_set_error('og_mailinglist_digest_interval_days',
      t('Digest interval be a positive integer.'));
  }
  
  $hour = $form_state['values']['og_mailinglist_digest_hour'];
  if (!is_numeric($hour)) {
    form_set_error('og_mailinglist_digest_hour',
      t('Digest hour must be a number.'));
  }
  elseif ($hour < 0 or $hour > 23) {
    form_set_error('og_mailinglist_digest_hour',
      t('Digest hour must be an integer between 0 and 23.'));
  }
}

/**
 * Admin settings form for configuration
 */
function og_mailinglist_config_settings() {
  $form['og_mailinglist_server_string'] = array(
    '#title' => t('Domain name'),
    '#type' => 'textfield',
    '#default_value' => variable_get('og_mailinglist_server_string', $_SERVER['SERVER_NAME']),
    '#description' => t('Domain name for outgoing/incoming emails, e.g. example.com.'),
  );
  // Save an incoming key by default.
  if ((variable_get('og_mailinglist_incoming_key', '')) == '') {
    variable_set('og_mailinglist_incoming_key',   md5(rand()));
  }
  $form['og_mailinglist_incoming_key'] = array(
    '#type' => 'textfield',
    '#title' => t('Message validation string'),
    '#default_value' => variable_get('og_mailinglist_incoming_key', md5(rand())),
    '#required' => TRUE,
    '#description' => t('This string will be used to validate incoming messages.  It can be anything, but must be used on both sides of the transfer. For more see the INSTALL.txt for your Mail Transfer Agent.'),
  );
  $form['og_mailinglist_test_email_address'] = array(
    '#title' => t('DEBUGGING: Enter one or more email addresses to send all emails'),
    '#type' => 'textfield',
    '#default_value' => variable_get('og_mailinglist_test_email_address', ''),
    '#description' => t('For testing / monitoring purposes, add one or more email addresses to receive all emails sent by OG Mailinglist. Separate multiple email addresses by comma.'),
  );
  return system_settings_form($form);
}

/**
 * Build subscription admin for for each user.
 */
function og_mailinglist_subscriptions_settings_form($form, $account) {
  global $user;
  $uid = $user->uid;
  if (is_object($account)) {
    $uid = $account->uid;
  }

  $result = db_query('SELECT m.nid as gid, m.subscription_type, n.title
          FROM {og_mailinglist_group_subscription} m, {node} n
          WHERE m.nid = n.nid
          AND m.uid = :uid
          ORDER BY n.title', array(':uid' => $uid));

  $subscription_options = array(
    'email' => t('Email'),
    'digest email' => t('Digest email'),
    'no email' => t('No email'),
  );
  $form = array();

  $form['uid'] = array(
    '#type' => 'value',
    '#value' => $uid,
  );

  foreach($result as $group) {
    $form[$group->gid] = array(
      '#type' => 'fieldset',
      '#title' => l(t($group->title), "node/" . $group->gid),
      '#tree' => TRUE,
    );
    $form[$group->gid]['subscription-type'] = array(
      '#type' => 'select',
      '#default_value' => $group->subscription_type,
      '#title' => t("Email Settings"),
      '#options' => $subscription_options,
    );
    $form[$group->gid]["leave-group"] = array(
      '#type' => 'checkbox',
      '#title' => t("Leave the @name group",
        array('@name' => t($group->title))),
      '#suffix' => "<hr />",
    );
  }
  $form['submit'] = array(
    '#type' => 'submit',
    '#value' => t('Save Settings'),
  );

  return $form;
}

/**
 * Form submit handler.
 */
function og_mailinglist_subscriptions_settings_form_submit($form, &$form_state) {
  $uid = $form_state['values']['uid'];
  unset($form_state['values']['uid']);
  foreach ($form_state['values'] as $gid => $settings) {
    if (is_numeric($gid)) {
      og_mailinglist_update_group_subcription_type($gid, $uid, $settings['subscription-type']);

      if ($settings['subscription-type']['#value'] != "email") {
        og_mailinglist_delete_group_threads($gid, $uid);
      }

      // If they choose to leave the group.
      if ($settings['leave-group']) {
        og_ungroup($gid, 'user', $uid);
        $group_name = $form[$gid]['title'];
        drupal_set_message(
          t("You are no longer in the <a href='@gid'>@group_name</a> group.",
          array(
            "@gid" => url("node/" . $gid),
            "@group_name" => $group_name,
          )));
      }
    }
  }
  drupal_set_message(t('Your group subscriptions were updated.'));
}